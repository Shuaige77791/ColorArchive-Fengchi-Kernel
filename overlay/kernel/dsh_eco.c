// SPDX-License-Identifier: GPL-2.0-only
/*
 * dsh_eco.c -- information-flow browsing power saver.
 *
 * While a feed app is in the foreground, cap the CPU frequency and suppress
 * boost, so the phone spends less power -- without making the UI stutter.
 *
 * The app list lives in userspace
 *   /sys/kernel/dsh_eco/apps is read/write.  The kernel only seeds it once, at
 *   boot, with a default list covering the mainstream feed apps; afterwards it
 *   is whatever was last written:
 *       cat  /sys/kernel/dsh_eco/apps
 *       echo "com.tencent.mm,com.sina.weibo" > /sys/kernel/dsh_eco/apps
 *   Entries are matched as prefixes against the process's *full* package name,
 *   read from /proc/<pid>/cmdline -- not comm, which is truncated to 15 chars
 *   and would make "com.ss.android.ugc.aweme" indistinguishable from Feishu.
 *
 * Mechanism: freq_qos FREQ_QOS_MAX.
 *   uclamp.max only lowers the utilisation a governor *sees*, and this device
 *   runs the `walt` governor -- a prebuilt vendor module whose uclamp handling
 *   cannot be verified.  FREQ_QOS_MAX is aggregated by the cpufreq core itself,
 *   so it holds for any governor, and the SoC's own thermal cooling devices
 *   already use exactly this interface.
 *
 * Detection (once a second):
 *   1. /dev/cpuctl/top-app/cgroup.procs non-empty  -> foreground app
 *   2. its package name matches an entry in `apps`
 *   3. /proc/game_opt/game_pid parses to -1        -> not a game
 *   4. its package name is not a launcher / SystemUI
 *   Every read is fail-safe: if we cannot tell, we do NOT cap.
 *
 * Safety:
 *   * little cluster untouched by default; only big + prime are capped
 *   * every cap clamped to [50%, 100%] of that cluster's cpuinfo max
 *   * state recomputed once a second, so a missed transition self-heals
 *   * 120 s grace period after boot
 *   * no task_struct field is added -- that would change struct layout and
 *     break the 462 prebuilt vendor modules via modversions CRC mismatch
 */
#define pr_fmt(fmt) "dsh_eco: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/jiffies.h>
#include <linux/ctype.h>
#include <linux/cpufreq.h>
#include <linux/pm_qos.h>
#include <linux/string.h>

#ifndef FREQ_QOS_MAX_DEFAULT_VALUE
#define FREQ_QOS_MAX_DEFAULT_VALUE	S32_MAX
#endif

#define DSH_ECO_PERIOD_MS	1000
#define DSH_ECO_FLOOR_PCT	50
#define DSH_ECO_MAX_CLUSTERS	4
#define DSH_ECO_GRACE_SEC	120
#define DSH_ECO_READ_MAX	512

#define DSH_APPS_BUF_MAX	8192
#define DSH_APPS_MAX		240
#define DSH_APP_LEN		64

struct dsh_eco_cluster {
	struct cpufreq_policy	*policy;
	struct freq_qos_request	qos;
	bool			qos_added;
	unsigned int		max_freq;
};

static struct dsh_eco_cluster dsh_clusters[DSH_ECO_MAX_CLUSTERS];
static int dsh_nr_clusters;

/* tunables */
static bool dsh_eco_enabled = true;
static unsigned int dsh_little_pct = 100;	/* little cluster untouched */
static unsigned int dsh_big_pct = 70;
static unsigned int dsh_prime_pct = 75;
static bool dsh_eco_force;

/* app list: seeded at boot, then owned by userspace */
static char dsh_apps_raw[DSH_APPS_BUF_MAX];
static char dsh_apps[DSH_APPS_MAX][DSH_APP_LEN];
static int dsh_apps_nr;

/* state (dsh_eco_active is read from kernel/sched/fair.c) */
bool dsh_eco_active;
static bool dsh_eco_last;
static unsigned int dsh_eco_reason;
static char dsh_fg_name[DSH_APP_LEN];
static struct delayed_work dsh_eco_work;
static struct kobject *dsh_eco_kobj;
static unsigned long dsh_eco_boot_jiffies;

/*
 * Default seed, matched as prefixes against the full package name.  Covers the
 * mainstream feed apps; userspace may replace the whole string at any time.
 */
static const char dsh_default_apps[] =
	/* 社交 feed */
	"com.tencent.mm,"
	"com.tencent.mobileqq,"
	"com.tencent.tim,"
	"com.tencent.qqlite,"
	"com.sina.weibo,"
	"com.xingin.xhs,"
	"com.zhihu.android,"
	"com.baidu.tieba,"
	"com.douban.frodo,"
	"com.immomo.momo,"
	"com.p1.mobile.putong,"
	"cn.soulapp.android,"
	"com.qzone,"
	/* 短视频 feed */
	"com.ss.android.ugc.aweme,"
	"com.ss.android.ugc.live,"
	"com.ss.android.ugc.pipixia,"
	"com.ss.android.article.video,"
	"com.smile.gifmaker,"
	"com.kuaishou.nebula,"
	"com.tencent.weishi,"
	"com.baidu.haokan,"
	"tv.danmaku.bili,"
	"com.bilibili.app.in,"
	"com.zhiliaoapp.musically,"
	"com.ss.android.ugc.trill,"
	/* 资讯新闻 */
	"com.ss.android.article.news,"
	"com.ss.android.article.lite,"
	"com.ss.android.article.ugc,"
	"com.tencent.news,"
	"com.netease.newsreader.activity,"
	"com.sina.news,"
	"com.sohu.newsclient,"
	"com.ifeng.news2,"
	"com.hupu.games,"
	"com.dongqiudi.news,"
	"com.tencent.qqsports,"
	/* 内容社区 / 浏览器 */
	"com.baidu.searchbox,"
	"com.UCMobile,"
	"com.tencent.mtt,"
	"com.izuiyou,"
	"com.qiushibaike,"
	"com.smzdm.client.android,"
	"com.autohome.main,"
	"com.netease.pm02,"
	/* 购物 feed */
	"com.taobao.taobao,"
	"com.tmall.wireless,"
	"com.jingdong.app.mall,"
	"com.xunmeng.pinduoduo,"
	"com.taobao.idlefish,"
	"com.shizhuang.duapp,"
	/* 阅读（长滚动） */
	"com.dragon.read,"
	"com.qidian.QDReader,"
	"com.tencent.weread,"
	"com.zhangyue.read,"
	"com.duokan.reader,"
	"com.ophone.reader.ui,"
	/* 国际 feed */
	"com.instagram.android,"
	"com.facebook.katana,"
	"com.facebook.lite,"
	"com.twitter.android,"
	"com.reddit.frontpage,"
	"com.linkedin.android,"
	"com.pinterest,"
	"com.snapchat.android,"
	"com.tumblr,"
	"com.google.android.youtube,"
	"com.threads.app,"
	"";

/* Never capped, whatever the app list says. */
static const char * const dsh_exempt_apps[] = {
	"com.android.launcher",
	"com.oplus.launcher",
	"com.coloros.launcher",
	"com.oppo.launcher",
	"com.oneplus.launcher",
	"com.google.android.apps.nexuslauncher",
	"com.android.systemui",
	"com.oplus.systemui",
	"com.coloros.systemui",
	NULL,
};

/* ------------------------------------------------------------------ */
/* app list                                                            */
/* ------------------------------------------------------------------ */

static bool dsh_in_list(const char *name, const char * const *list)
{
	const char * const *p;

	if (!name || !name[0])
		return false;

	for (p = list; *p; p++) {
		if (!strncmp(name, *p, strlen(*p)))
			return true;
	}
	return false;
}

static bool dsh_in_apps(const char *name)
{
	int i;

	if (!name || !name[0])
		return false;

	for (i = 0; i < dsh_apps_nr; i++) {
		if (!strncmp(name, dsh_apps[i], strlen(dsh_apps[i])))
			return true;
	}
	return false;
}

static void dsh_apps_parse(void)
{
	char *s = dsh_apps_raw;

	dsh_apps_nr = 0;

	while (*s && dsh_apps_nr < DSH_APPS_MAX) {
		char *start;
		size_t len;

		while (*s && (*s == ',' || *s == ';' || isspace(*s)))
			s++;
		if (!*s)
			break;

		start = s;
		while (*s && *s != ',' && *s != ';' && !isspace(*s))
			s++;

		len = (size_t)(s - start);
		if (len == 0 || len >= DSH_APP_LEN)
			continue;

		memcpy(dsh_apps[dsh_apps_nr], start, len);
		dsh_apps[dsh_apps_nr][len] = '\0';
		dsh_apps_nr++;
	}
}

/* ------------------------------------------------------------------ */
/* small procfs / cgroup reads                                         */
/* ------------------------------------------------------------------ */

static int dsh_read_small(const char *path, char *buf, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);

	n = kernel_read(f, buf, len - 1, &pos);
	filp_close(f, NULL);

	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	return (int)n;
}

/* full package name from cmdline, falling back to comm */
static int dsh_read_pkgname(pid_t pid, char *out, size_t len)
{
	char path[64];
	size_t i;
	int n;

	scnprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	n = dsh_read_small(path, out, len);
	if (n > 0) {
		for (i = 0; i < (size_t)n; i++) {
			if (out[i] == '\0')
				break;
		}
		out[i] = '\0';
		if (i > 0)
			return (int)i;
	}

	scnprintf(path, sizeof(path), "/proc/%d/comm", pid);
	n = dsh_read_small(path, out, len);
	if (n <= 0)
		return -1;

	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return n;
}

/*
 * 1 = a listed feed app is in the foreground, 0 = some other app,
 * -1 = unknown.  The first non-exempt package name is kept in dsh_fg_name.
 */
static int dsh_foreground_is_feed(void)
{
	char buf[DSH_ECO_READ_MAX];
	char name[DSH_APP_LEN];
	const char *s;
	bool any = false;
	bool feed = false;

	dsh_fg_name[0] = '\0';

	if (dsh_read_small("/dev/cpuctl/top-app/cgroup.procs",
			   buf, sizeof(buf)) <= 0)
		return -1;

	s = buf;
	while (*s) {
		char *end;
		long pid;

		while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
			s++;
		if (!*s)
			break;

		pid = simple_strtol(s, &end, 10);
		if (end == s)
			break;
		s = end;
		any = true;

		if (pid <= 0)
			continue;
		if (dsh_read_pkgname((pid_t)pid, name, sizeof(name)) <= 0)
			continue;

		if (dsh_in_list(name, dsh_exempt_apps)) {
			strscpy(dsh_fg_name, name, sizeof(dsh_fg_name));
			return 0;
		}
		if (!feed && dsh_in_apps(name)) {
			feed = true;
			strscpy(dsh_fg_name, name, sizeof(dsh_fg_name));
		}
	}

	if (!any)
		return -1;
	return feed ? 1 : 0;
}

/* 1 = game running, 0 = no game, -1 = unknown */
static int dsh_game_running(void)
{
	char buf[128];
	const char *p;
	long pid;
	int n;

	n = dsh_read_small("/proc/game_opt/game_pid", buf, sizeof(buf));
	if (n <= 0)
		return -1;

	p = strstr(buf, "game_pid=");
	if (!p)
		return -1;
	p += strlen("game_pid=");

	if (kstrtol(p, 10, &pid))
		return -1;

	return (pid < 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* frequency capping                                                   */
/* ------------------------------------------------------------------ */

static void dsh_apply(bool active)
{
	int i;

	for (i = 0; i < dsh_nr_clusters; i++) {
		struct dsh_eco_cluster *c = &dsh_clusters[i];
		unsigned int pct;
		s32 target;

		if (!c->qos_added)
			continue;

		if (!active) {
			freq_qos_update_request(&c->qos,
						FREQ_QOS_MAX_DEFAULT_VALUE);
			continue;
		}

		if (i == 0)
			pct = dsh_little_pct;
		else if (i == dsh_nr_clusters - 1)
			pct = dsh_prime_pct;
		else
			pct = dsh_big_pct;

		if (pct > 100)
			pct = 100;
		if (pct < DSH_ECO_FLOOR_PCT)
			pct = DSH_ECO_FLOOR_PCT;

		target = (s32)((u64)c->max_freq * pct / 100);
		freq_qos_update_request(&c->qos, target);
	}
}

/* ------------------------------------------------------------------ */
/* state machine                                                       */
/* ------------------------------------------------------------------ */

static void dsh_eco_worker(struct work_struct *work)
{
	bool want;

	if (time_before(jiffies, dsh_eco_boot_jiffies +
			DSH_ECO_GRACE_SEC * HZ)) {
		dsh_eco_reason = 4;
		want = false;
		goto apply;
	}

	if (dsh_eco_force) {
		dsh_eco_reason = 3;
		want = true;
		goto apply;
	}

	if (!dsh_eco_enabled || dsh_apps_nr == 0) {
		dsh_eco_reason = 0;
		want = false;
		goto apply;
	}

	if (dsh_game_running() != 0) {
		dsh_eco_reason = 2;
		want = false;
		goto apply;
	}

	if (dsh_foreground_is_feed() != 1) {
		/* 0 = some other app, -1 = unknown: never cap either way */
		dsh_eco_reason = 0;
		want = false;
		goto apply;
	}

	dsh_eco_reason = 1;
	want = true;

apply:
	dsh_eco_active = want;
	if (want != dsh_eco_last) {
		dsh_apply(want);
		pr_info("%s reason=%u app=%s (little=%u%% big=%u%% prime=%u%%)\n",
			want ? "ON" : "OFF", dsh_eco_reason,
			dsh_fg_name[0] ? dsh_fg_name : "-",
			dsh_little_pct, dsh_big_pct, dsh_prime_pct);
		dsh_eco_last = want;
	} else if (want) {
		dsh_apply(true);
	}

	schedule_delayed_work(&dsh_eco_work,
			      msecs_to_jiffies(DSH_ECO_PERIOD_MS));
}

/* ------------------------------------------------------------------ */
/* sysfs                                                               */
/* ------------------------------------------------------------------ */

static ssize_t enabled_show(struct kobject *k, struct kobj_attribute *a, char *b)
{ return scnprintf(b, PAGE_SIZE, "%d\n", dsh_eco_enabled); }
static ssize_t enabled_store(struct kobject *k, struct kobj_attribute *a,
			     const char *buf, size_t n)
{ int v; if (kstrtoint(buf, 10, &v)) return -EINVAL; dsh_eco_enabled = !!v; return n; }
static struct kobj_attribute attr_enabled =
	__ATTR(enabled, 0644, enabled_show, enabled_store);

static ssize_t force_show(struct kobject *k, struct kobj_attribute *a, char *b)
{ return scnprintf(b, PAGE_SIZE, "%d\n", dsh_eco_force); }
static ssize_t force_store(struct kobject *k, struct kobj_attribute *a,
			   const char *buf, size_t n)
{ int v; if (kstrtoint(buf, 10, &v)) return -EINVAL; dsh_eco_force = !!v; return n; }
static struct kobj_attribute attr_force =
	__ATTR(force, 0644, force_show, force_store);

static ssize_t apps_show(struct kobject *k, struct kobj_attribute *a, char *b)
{
	return scnprintf(b, PAGE_SIZE, "%s\n# %d entries\n",
			 dsh_apps_raw, dsh_apps_nr);
}
static ssize_t apps_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t n)
{
	if (n >= DSH_APPS_BUF_MAX)
		return -EINVAL;

	memset(dsh_apps_raw, 0, sizeof(dsh_apps_raw));
	memcpy(dsh_apps_raw, buf, n);
	dsh_apps_raw[n] = '\0';
	dsh_apps_parse();

	pr_info("app list replaced: %d entries\n", dsh_apps_nr);
	return n;
}
static struct kobj_attribute attr_apps =
	__ATTR(apps, 0644, apps_show, apps_store);

static ssize_t state_show(struct kobject *k, struct kobj_attribute *a, char *b)
{
	static const char * const why[] = {
		"idle", "feed-app", "game", "forced", "boot-grace",
	};

	return scnprintf(b, PAGE_SIZE,
		"active=%d reason=%s\n"
		"foreground=%s\n"
		"feed_app=%d game=%d entries=%d\n"
		"little=%u%% big=%u%% prime=%u%% clusters=%d\n",
		dsh_eco_active,
		why[dsh_eco_reason < ARRAY_SIZE(why) ? dsh_eco_reason : 0],
		dsh_fg_name[0] ? dsh_fg_name : "-",
		dsh_foreground_is_feed(), dsh_game_running(), dsh_apps_nr,
		dsh_little_pct, dsh_big_pct, dsh_prime_pct, dsh_nr_clusters);
}
static struct kobj_attribute attr_state =
	__ATTR(state, 0444, state_show, NULL);

#define PCT_ATTR(_name, _var)						\
static ssize_t _name##_show(struct kobject *k, struct kobj_attribute *a, char *b)\
{ return scnprintf(b, PAGE_SIZE, "%u\n", _var); }			\
static ssize_t _name##_store(struct kobject *k, struct kobj_attribute *a,	\
			     const char *buf, size_t n)			\
{									\
	unsigned int v;							\
	if (kstrtouint(buf, 10, &v))					\
		return -EINVAL;						\
	if (v < DSH_ECO_FLOOR_PCT || v > 100)				\
		return -EINVAL;						\
	_var = v;							\
	return n;							\
}									\
static struct kobj_attribute attr_##_name =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

PCT_ATTR(little_pct, dsh_little_pct);
PCT_ATTR(big_pct,    dsh_big_pct);
PCT_ATTR(prime_pct,  dsh_prime_pct);

static struct attribute *dsh_eco_attrs[] = {
	&attr_enabled.attr,
	&attr_force.attr,
	&attr_state.attr,
	&attr_apps.attr,
	&attr_little_pct.attr,
	&attr_big_pct.attr,
	&attr_prime_pct.attr,
	NULL,
};
static struct attribute_group dsh_eco_group = { .attrs = dsh_eco_attrs };

/* ------------------------------------------------------------------ */
/* cluster discovery                                                   */
/* ------------------------------------------------------------------ */

static int dsh_setup_clusters(void)
{
	struct cpufreq_policy *seen[DSH_ECO_MAX_CLUSTERS] = { NULL };
	unsigned int cpu;
	int i, j, n = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *pol = cpufreq_cpu_get(cpu);
		bool dup = false;

		if (!pol)
			continue;

		for (i = 0; i < n; i++) {
			if (seen[i] == pol) {
				dup = true;
				break;
			}
		}
		if (dup || n >= DSH_ECO_MAX_CLUSTERS) {
			cpufreq_cpu_put(pol);
			continue;
		}
		seen[n++] = pol;
	}

	if (n == 0)
		return -ENODEV;

	for (i = 0; i < n - 1; i++) {
		for (j = 0; j < n - 1 - i; j++) {
			if (seen[j]->cpuinfo.max_freq >
			    seen[j + 1]->cpuinfo.max_freq) {
				struct cpufreq_policy *t = seen[j];
				seen[j] = seen[j + 1];
				seen[j + 1] = t;
			}
		}
	}

	for (i = 0; i < n; i++) {
		dsh_clusters[i].policy = seen[i];
		dsh_clusters[i].max_freq = seen[i]->cpuinfo.max_freq;
	}
	dsh_nr_clusters = n;
	return 0;
}

static void dsh_add_qos(void)
{
	int i;

	for (i = 0; i < dsh_nr_clusters; i++) {
		struct dsh_eco_cluster *c = &dsh_clusters[i];

		if (freq_qos_add_request(&c->policy->constraints, &c->qos,
					 FREQ_QOS_MAX,
					 FREQ_QOS_MAX_DEFAULT_VALUE) < 0) {
			pr_warn("cluster %d: freq_qos_add_request failed\n", i);
			continue;
		}
		c->qos_added = true;
	}
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

static int __init dsh_eco_init(void)
{
	int ret, i;

	dsh_eco_boot_jiffies = jiffies;

	ret = dsh_setup_clusters();
	if (ret) {
		pr_warn("no cpufreq policy (%d); feature disabled\n", ret);
		return 0;
	}

	for (i = 0; i < dsh_nr_clusters; i++)
		pr_info("cluster %d: max %u kHz\n", i, dsh_clusters[i].max_freq);

	dsh_add_qos();

	/* seed the list; userspace owns it from here */
	strscpy(dsh_apps_raw, dsh_default_apps, sizeof(dsh_apps_raw));
	dsh_apps_parse();

	dsh_eco_kobj = kobject_create_and_add("dsh_eco", kernel_kobj);
	if (dsh_eco_kobj) {
		if (sysfs_create_group(dsh_eco_kobj, &dsh_eco_group))
			pr_warn("sysfs group failed\n");
	} else {
		pr_warn("kobject failed\n");
	}

	INIT_DELAYED_WORK(&dsh_eco_work, dsh_eco_worker);
	schedule_delayed_work(&dsh_eco_work, msecs_to_jiffies(5000));

	pr_info("ready: %d apps, little=%u%% big=%u%% prime=%u%%, %d s grace\n",
		dsh_apps_nr, dsh_little_pct, dsh_big_pct, dsh_prime_pct,
		DSH_ECO_GRACE_SEC);
	return 0;
}

late_initcall(dsh_eco_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DSH information-flow browsing power saver");
