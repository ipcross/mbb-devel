/* Copyright (C) 2010 Mikhail Osipov <mike.osipov@gmail.com> */
/* Published under the GNU General Public License V.2, see file COPYING */

#include "mbbsession.h"
#include "mbbthread.h"
#include "mbbxmlmsg.h"
#include "mbblock.h"
#include "mbbauth.h"
#include "mbbinit.h"
#include "mbbfunc.h"
#include "mbblog.h"
#include "mbbvar.h"
#include "mbbxtv.h"

#include "mbbplock.h"

#include "macros.h"

#include <time.h>

#define G_STRUCT_OFFPTR(type, member) GINT_TO_POINTER(G_STRUCT_OFFSET(type, member))

/* PROHIBITED calling mbb_log inside the lock */

static guint session_id = 1;
static GHashTable *ht = NULL;

static GQueue var_queue = G_QUEUE_INIT;

static struct mbb_var *show_mtime_var = NULL;

struct show_sessions_data {
	XmlTag *ans;
	gboolean show_mtime;
};

void mbb_session_add_var(struct mbb_var *var)
{
	mbb_plock_writer_lock();
	g_queue_push_tail(&var_queue, var);
	mbb_plock_writer_unlock();
}

static inline gboolean session_lookup_var(struct mbb_session *ss,
					  struct mbb_var *var,
					  gpointer *data)
{
	return g_hash_table_lookup_extended(ss->vars, var, NULL, data);
}

void mbb_session_del_var(struct mbb_var *var)
{
	mbb_plock_writer_lock();

	g_queue_remove(&var_queue, var);

	if (ht != NULL) {
		struct mbb_session_var *priv;
		struct mbb_session *ss;
		GHashTableIter iter;

		priv = mbb_var_get_priv(var);

		g_hash_table_iter_init(&iter, ht);
		while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &ss)) {
			gpointer data;

			if (session_lookup_var(ss, var, &data)) {
				if (priv->op_free != NULL)
					priv->op_free(data);

				g_hash_table_remove(ss->vars, var);
			}
		}
	}

	mbb_plock_writer_unlock();
}

static gpointer session_insert_var(struct mbb_session *ss, struct mbb_var *var)
{
	struct mbb_session_var *priv;
	gpointer data;

	priv = mbb_var_get_priv(var);

	if (priv->op_new == NULL)
		data = priv->data;
	else
		data = priv->op_new(priv->data);
		
	g_hash_table_insert(ss->vars, var, data);

	return data;
}

gpointer mbb_session_var_get_data(struct mbb_var *var)
{
	struct mbb_session *ss;
	gpointer data;

	if ((ss = current_session()) == NULL)
		return NULL;

	if (session_lookup_var(ss, var, &data) == FALSE)
		data = session_insert_var(ss, var);

	return data;
}

static void session_vars_init(struct mbb_session *ss)
{
	GList *list;

	ss->vars = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (list = var_queue.head; list != NULL; list = list->next)
		session_insert_var(ss, list->data);
}

static void session_vars_fini(struct mbb_session *ss)
{
	GHashTableIter iter;
	struct mbb_var *var;
	gpointer data;

	g_hash_table_iter_init(&iter, ss->vars);
	while (g_hash_table_iter_next(&iter, (gpointer *) &var, &data)) {
		struct mbb_session_var *priv;

		priv = mbb_var_get_priv(var);
		if (priv->op_free != NULL)
			priv->op_free(data);
	}

	g_hash_table_destroy(ss->vars);
}

static void mbb_session_free(struct mbb_session *ss)
{
	session_vars_fini(ss);

	if (ss->user != NULL)
		mbb_user_unref(ss->user);

	g_free(ss->kill_msg);
	g_free(ss->peer);

	if (ss->cached_vars != NULL)
		g_hash_table_destroy(ss->cached_vars);
}

static inline void ht_init(void)
{
	ht = g_hash_table_new_full(
		g_direct_hash, g_direct_equal,
		NULL, (GDestroyNotify) mbb_session_free
	);
}

guint mbb_session_new(struct mbb_session *ss, gchar *peer, guint port, mbb_session_type type)
{
	guint sid;

	ss->user = NULL;
	ss->peer = g_strdup(peer);
	ss->port = port;
	ss->type = type;

	time(&ss->start);
	ss->mtime = ss->start;

	ss->killed = FALSE;
	ss->kill_msg = NULL;

	ss->cached_vars = NULL;

	session_vars_init(ss);

	mbb_plock_writer_lock();

	sid = session_id++;
	if (ht == NULL)
		ht_init();

	g_hash_table_insert(ht, GINT_TO_POINTER(sid), ss);

	mbb_plock_writer_unlock();

	return sid;
}

void mbb_session_touch(struct mbb_session *ss)
{
	time(&ss->mtime);
}

gboolean mbb_session_auth(struct mbb_session *ss, gchar *login, gchar *secret,
			  gchar *type)
{
	MbbUser *user;

	mbb_lock_reader_lock();
	user = mbb_auth(login, secret, type);
	if (user != NULL) {
		if (ss->user != NULL)
			mbb_user_unref(ss->user);

		ss->user = mbb_user_ref(user);
		mbb_log("auth as %s", user->name);
	}
	mbb_lock_reader_unlock();

	return user != NULL;
}

gboolean mbb_session_has(gint sid)
{
	gboolean ret = FALSE;

	mbb_plock_reader_lock();

	if (ht != NULL && g_hash_table_lookup(ht, GINT_TO_POINTER(sid)) != NULL)
		ret = TRUE;

	mbb_plock_reader_unlock();

	return ret;
}

void mbb_session_quit(guint sid)
{
	mbb_plock_writer_lock();

	if (ht != NULL)
		g_hash_table_remove(ht, GINT_TO_POINTER(sid));

	mbb_plock_writer_unlock();
}

gboolean mbb_session_is_http(void)
{
	struct mbb_session *ss;

	if ((ss = current_session()) == NULL)
		return FALSE;

	return ss->type == MBB_SESSION_HTTP;
}

GHashTable *mbb_session_get_cached(gboolean create)
{
	struct mbb_session *ss;

	if ((ss = current_session()) == NULL)
		return NULL;

	if (create == FALSE)
		return ss->cached_vars;

	if (ss->cached_vars == NULL) {
                ss->cached_vars = g_hash_table_new_full(
                        g_str_hash, g_str_equal, g_free, g_free
                );
        }

	return ss->cached_vars;
}

static void gather_session(gpointer key, gpointer value, gpointer data)
{
	struct show_sessions_data *ssd;
	struct mbb_session *ss;
	gchar *username;
	XmlTag *xt;
	guint sid;

	sid = GPOINTER_TO_INT(key);
	ss = (struct mbb_session *) value;
	ssd = (struct show_sessions_data *) data;

	if (ss->user == NULL)
		username = g_strdup("*");
	else {
		mbb_user_lock(ss->user);
		username = g_strdup(ss->user->name);
		mbb_user_unlock(ss->user);
	}

	xml_tag_add_child(ssd->ans, xt = xml_tag_newc(
		"session",
		"sid", variant_new_int(sid),
		"user", variant_new_alloc_string(username),
		"peer", variant_new_string(ss->peer),
		"start", variant_new_alloc_string(g_strdup_printf("%ld", ss->start))
	));

	if (ssd->show_mtime) {
		gchar *mtime = g_strdup_printf("%ld", ss->mtime);
		xml_tag_set_attr(xt, "mtime", variant_new_alloc_string(mtime));
	}
}

static void show_sessions(XmlTag *tag G_GNUC_UNUSED, XmlTag **ans)
{
	mbb_plock_reader_lock();

	if (ht != NULL) {
		struct show_sessions_data ssd;

		*ans = mbb_xml_msg_ok();
		ssd.ans = *ans;
		ssd.show_mtime = *(gboolean *) mbb_session_var_get_data(show_mtime_var);

		g_hash_table_foreach(ht, gather_session, &ssd);
	}

	mbb_plock_reader_unlock();
}

static gchar *var_session_peer_get(gpointer p G_GNUC_UNUSED)
{
	return g_strdup(mbb_thread_get_peer());
}

static gchar *var_session_user_get(gpointer p G_GNUC_UNUSED)
{
	gchar *username = NULL;
	MbbUser *user;

	user = mbb_thread_get_user();
	if (user == NULL)
		username = g_strdup("*");
	else {
		mbb_user_lock(user);
		username = g_strdup(user->name);
		mbb_user_unlock(user);
	}

	return username;
}

static void kill_session(XmlTag *tag, XmlTag **ans)
{
	DEFINE_XTV(XTV_SESSION_SID);

	struct mbb_session *current = NULL;
	struct mbb_session *ss = NULL;
	guint sid = -1;

	MBB_XTV_CALL(&sid);

	current = current_session();

	mbb_plock_reader_lock();

	on_final { mbb_plock_reader_unlock(); }

	if (ht != NULL)
		ss = g_hash_table_lookup(ht, GINT_TO_POINTER(sid));

	if (ss == NULL) final
		*ans = mbb_xml_msg(MBB_MSG_UNKNOWN_SESSION);

	ss->killed = TRUE;
	if (current != NULL && current->user != NULL) {
		if (ss->kill_msg != NULL)
			g_free(ss->kill_msg);

		mbb_user_lock(current->user);
		ss->kill_msg = g_strdup_printf("killed by %s", current->user->name);
		mbb_user_unlock(current->user);
	}

	mbb_plock_reader_unlock();

	mbb_thread_raise(sid);
}

MBB_VAR_DEF(session_peer) {
	.op_read = var_session_peer_get,
	.cap_read = MBB_CAP_ALL
};

MBB_VAR_DEF(session_user) {
	.op_read = var_session_user_get,
	.cap_read = MBB_CAP_ALL
};

static gboolean session_show_mtime__ = FALSE;

MBB_VAR_DEF(ss_show_mtime_def) {
	.op_read = var_str_bool,
	.op_write = var_conv_bool,
	.cap_read = MBB_CAP_WHEEL,
	.cap_write = MBB_CAP_WHEEL
};

MBB_SESSION_VAR_DEF(ss_show_mtime) {
	.op_new = g_ptr_booldup,
	.op_free = g_free,
	.data = &session_show_mtime__
};

MBB_INIT_FUNCTIONS_DO
	MBB_FUNC_STRUCT("mbb-show-sessions", show_sessions, MBB_CAP_ADMIN),
	MBB_FUNC_STRUCT("mbb-kill-session", kill_session, MBB_CAP_WHEEL),
MBB_INIT_FUNCTIONS_END

static void init_vars(void)
{
	mbb_base_var_register(SS_("peer"), &session_peer, NULL);
	mbb_base_var_register(SS_("user"), &session_user, NULL);

	show_mtime_var = mbb_session_var_register(
		SS_("session.show.mtime"), &ss_show_mtime_def, &ss_show_mtime
	);
}

MBB_ON_INIT(MBB_INIT_VARS, MBB_INIT_FUNCTIONS)
