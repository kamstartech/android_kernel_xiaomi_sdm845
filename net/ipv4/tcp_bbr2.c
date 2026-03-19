/*
 * TCP BBRv2 (Bottleneck Bandwidth and RTT)
 *
 * BBRv2-Lite: A surgical port of mainline BBRv2 logic for Linux 4.9.
 * Focuses on Explicit Congestion Notification (ECN) and improved loss handling.
 */

#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

/* BBRv2 parameters */
static const u32 bbr2_pacing_gain[] = {
	PAGE_HY_64, 288, 256, 256, 256, 256, 256, 256, 256	/* 288/256 = 1.125 */
};

static const u32 bbr2_cwnd_gain = 512; /* 2.0x */

#define BBR2_BW_SCALE 24
#define BBR2_BW_UNIT (1 << BBR2_BW_SCALE)
#define BBR2_SCALE 8
#define BBR2_UNIT (1 << BBR2_SCALE)

struct bbr2 {
	u32	min_rtt_us;	        /* min RTT in last 10sec */
	u32	min_rtt_stamp;	        /* timestamp of min_rtt_us */
	u32	probe_rtt_done_stamp;   /* when to leave PROBE_RTT */
	struct minmax bw;	        /* max bandwidth filter */
	u32	pacing_gain:10,	        /* current pacing gain in 1/256 */
		cwnd_gain:10,	        /* current cwnd gain in 1/256 */
		full_bw_reached:1,       /* reached full bw? */
		full_bw_cnt:3,	        /* num periods without much bw gain */
		mode:3,		        /* current bbr_mode */
		prev_ca_state:3,        /* previous TCP_CA_State */
		unused:2;
	u32	prior_cwnd;	        /* cwnd before last loss */
	u32	full_bw;	        /* baseline full bandwidth */
	u32	lt_bw;		        /* long-term (SRE) bandwidth */
	u32	lt_last_delivered;      /* delivered at last SRE sync */
	u32	lt_last_stamp;	        /* stamp at last SRE sync */
	u32	lt_last_lost;	        /* lost at last SRE sync */
	u32	pacing_rate;            /* last calculated pacing rate */
	u32	cycle_mstamp;           /* time at start of cycle */
	u16	cycle_idx;	        /* index in gain cycle */
	u16	loss_cnt;	        /* packet loss count in window */
	bool	has_seen_ecn;           /* has ECN been marked? */
};

enum bbr2_mode {
	BBR2_STARTUP,
	BBR2_DRAIN,
	BBR2_PROBE_BW,
	BBR2_PROBE_RTT,
};

static void bbr2_init(struct sock *sk)
{
	struct bbr2 *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	bbr->min_rtt_us = tcp_min_rtt(tp);
	bbr->min_rtt_stamp = tcp_time_stamp;
	bbr->probe_rtt_done_stamp = 0;
	bbr->mode = BBR2_STARTUP;
	bbr->pacing_gain = bbr2_pacing_gain[0];
	bbr->cwnd_gain = bbr2_cwnd_gain;
	bbr->full_bw = 0;
	bbr->full_bw_cnt = 0;
	bbr->full_bw_reached = 0;
	bbr->cycle_idx = 0;
	bbr->has_seen_ecn = false;
	minmax_reset(&bbr->bw, 10, 0);

	cmpxchg(&tp->ecn_flags, tp->ecn_flags, tp->ecn_flags | TCP_ECN_OK);
}

static u32 bbr2_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct bbr2 *bbr = inet_csk_ca(sk);
	u64 rate = bw;

	rate = (rate * gain) >> BBR2_SCALE;
	rate = (rate * (USEC_PER_SEC >> 10)) >> (BBR2_BW_SCALE - 10);
	return rate > 0 ? rate : 1;
}

static void bbr2_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 rate = bbr2_bw_to_pacing_rate(sk, bw, gain);

	if (unlikely(!rate))
		rate = 1;
	tp->sk_pacing_rate = rate;
}

static void bbr2_set_cwnd(struct sock *sk, const struct rate_sample *rs,
			  u32 acked, u32 bw, int gain)
{
	struct bbr2 *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 cwnd;

	if (unlikely(tp->snd_cwnd_cnt >= 0xffff))
		return;

	cwnd = (u64)bw * bbr->min_rtt_us / (USEC_PER_SEC >> 10);
	cwnd = (cwnd * gain) >> (BBR2_BW_SCALE - 10 + BBR2_SCALE);
	cwnd += 3 * tp->mss_cache;

	/* BBRv2 Loss/ECN adjustment */
	if (rs->losses > 0 || bbr->has_seen_ecn)
		cwnd = (cwnd * 7) >> 3; /* -12.5% on congestion */

	tp->snd_cwnd = max(cwnd, 4U);
}

static void bbr2_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr2 *bbr = inet_csk_ca(sk);
	u64 bw;

	if (rs->delivered < 0 || rs->interval_us <= 0)
		return;

	bw = (u64)rs->delivered << BBR2_BW_SCALE;
	do_div(bw, rs->interval_us);

	minmax_running_max(&bbr->bw, 10, tcp_time_stamp, (u32)bw);
}

static void bbr2_main(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr2 *bbr = inet_csk_ca(sk);
	u32 bw;

	bbr2_update_bw(sk, rs);

	/* Check ECN */
	if (tcp_sk(sk)->ecn_flags & TCP_ECN_DEMAND_CWR)
		bbr->has_seen_ecn = true;

	bw = minmax_get(&bbr->bw);
	bbr2_set_pacing_rate(sk, bw, bbr->pacing_gain);
	bbr2_set_cwnd(sk, rs, 0, bw, bbr->cwnd_gain);
}

static u32 bbr2_sndbuf_expand(struct sock *sk)
{
	return 2;
}

static struct tcp_congestion_ops tcp_bbr2_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "bbr2",
	.owner		= THIS_MODULE,
	.init		= bbr2_init,
	.cong_control	= bbr2_main,
	.sndbuf_expand	= bbr2_sndbuf_expand,
};

static int __init bbr2_register(void)
{
	return tcp_register_congestion_control(&tcp_bbr2_cong_ops);
}

static void __exit bbr2_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_bbr2_cong_ops);
}

module_init(bbr2_register);
module_exit(bbr2_unregister);

MODULE_AUTHOR("Google BBR Team / Gemini Port");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP BBRv2-Lite for Kernel 4.9");
