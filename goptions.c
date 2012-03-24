#include <locale.h>
#include <malloc.h>
#include <string.h>

#include <glib.h>
#include <cairo.h>
#include <gtk/gtk.h>

#include "fio.h"
#include "gfio.h"
#include "ghelpers.h"
#include "parse.h"

struct gopt {
	GtkWidget *box;
	unsigned int opt_index;
	unsigned int opt_type;
	gulong sig_handler;
	struct gopt_job_view *gjv;
};

struct gopt_combo {
	struct gopt gopt;
	GtkWidget *combo;
};

struct gopt_int {
	struct gopt gopt;
	unsigned int lastval;
	GtkWidget *spin;
};

struct gopt_bool {
	struct gopt gopt;
	GtkWidget *check;
};

struct gopt_str {
	struct gopt gopt;
	GtkWidget *entry;
};

struct gopt_str_val {
	struct gopt gopt;
	GtkWidget *spin;
	GtkWidget *combo;
	unsigned int maxindex;
};

#define GOPT_RANGE_SPIN	4

struct gopt_range {
	struct gopt gopt;
	GtkWidget *spins[GOPT_RANGE_SPIN];
};

struct gopt_str_multi {
	struct gopt gopt;
	GtkWidget *checks[PARSE_MAX_VP];
};

struct gopt_frame_widget {
	GtkWidget *vbox[2];
	unsigned int nr;
};

struct gopt_job_view {
	struct flist_head list;
	struct gopt_frame_widget g_widgets[__FIO_OPT_G_NR];
	GtkWidget *widgets[FIO_MAX_OPTS];
	GtkWidget *vboxes[__FIO_OPT_C_NR];
};

static GNode *gopt_dep_tree;

static GtkWidget *gopt_get_group_frame(struct gopt_job_view *gjv,
				       GtkWidget *box, unsigned int groupmask)
{
	unsigned int mask, group;
	struct opt_group *og;
	GtkWidget *frame, *hbox;
	struct gopt_frame_widget *gfw;

	if (!groupmask)
		return 0;

	mask = groupmask;
	og = opt_group_cat_from_mask(&mask);
	if (!og)
		return NULL;

	group = ffz(~groupmask);
	gfw = &gjv->g_widgets[group];
	if (!gfw->vbox[0]) {
		frame = gtk_frame_new(og->name);
		gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 3);
		hbox = gtk_hbox_new(FALSE, 0);
		gtk_container_add(GTK_CONTAINER(frame), hbox);
		gfw->vbox[0] = gtk_vbox_new(TRUE, 5);
		gfw->vbox[1] = gtk_vbox_new(TRUE, 5);
		gtk_box_pack_start(GTK_BOX(hbox), gfw->vbox[0], TRUE, TRUE, 5);
		gtk_box_pack_start(GTK_BOX(hbox), gfw->vbox[1], TRUE, TRUE, 5);
	}

	hbox = gtk_hbox_new(FALSE, 3);
	gtk_box_pack_start(GTK_BOX(gfw->vbox[gfw->nr++ & 1]), hbox, FALSE, FALSE, 5);
	return hbox;
}

/*
 * Mark children as invisible, if needed.
 */
static void gopt_set_children_visible(struct fio_option *parent,
				      gboolean visible)
{
	GNode *child, *node;

	if (parent->hide_on_set)
		visible = !visible;

	node = g_node_find(gopt_dep_tree, G_IN_ORDER, G_TRAVERSE_ALL, parent);
	child = g_node_first_child(node);
	while (child) {
		struct fio_option *o = child->data;
		struct gopt *g = o->gui_data;
		struct gopt_job_view *gjv = g->gjv;

		/*
		 * Recurse into child, if it also has children
		 */
		if (g_node_n_children(child))
			gopt_set_children_visible(o, visible);

		if (gjv->widgets[g->opt_index])
			gtk_widget_set_sensitive(gjv->widgets[g->opt_index], visible);

		child = g_node_next_sibling(child);
	}
}

static void gopt_str_changed(GtkEntry *entry, gpointer data)
{
	struct gopt_str *s = (struct gopt_str *) data;
	struct fio_option *o = &fio_options[s->gopt.opt_index];
	const gchar *text;
	int set;

	text = gtk_entry_get_text(GTK_ENTRY(s->entry));
	set = strcmp(text, "") != 0;
	gopt_set_children_visible(o, set);
}

static void gopt_mark_index(struct gopt_job_view *gjv, struct gopt *gopt,
			    unsigned int idx)
{
	assert(!gjv->widgets[idx]);
	gopt->opt_index = idx;
	gopt->gjv = gjv;
	gjv->widgets[idx] = gopt->box;
}

static void gopt_str_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_str *s = (struct gopt_str *) data;

	free(s);
	gtk_widget_destroy(w);
}

static struct gopt *gopt_new_str_store(struct gopt_job_view *gjv,
				       struct fio_option *o, const char *text,
				       unsigned int idx)
{
	struct gopt_str *s;
	GtkWidget *label;

	s = malloc(sizeof(*s));
	memset(s, 0, sizeof(*s));

	s->gopt.box = gtk_hbox_new(FALSE, 3);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);

	s->entry = gtk_entry_new();
	gopt_mark_index(gjv, &s->gopt, idx);
	if (text)
		gtk_entry_set_text(GTK_ENTRY(s->entry), text);
	gtk_entry_set_editable(GTK_ENTRY(s->entry), 1);

	if (o->def)
		gtk_entry_set_text(GTK_ENTRY(s->entry), o->def);

	s->gopt.sig_handler = g_signal_connect(GTK_OBJECT(s->entry), "changed", G_CALLBACK(gopt_str_changed), s);
	g_signal_connect(GTK_OBJECT(s->entry), "destroy", G_CALLBACK(gopt_str_destroy), s);

	gtk_box_pack_start(GTK_BOX(s->gopt.box), s->entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(s->gopt.box), label, FALSE, FALSE, 0);
	return &s->gopt;
}

static void gopt_combo_changed(GtkComboBox *box, gpointer data)
{
	struct gopt_combo *c = (struct gopt_combo *) data;
	struct fio_option *o = &fio_options[c->gopt.opt_index];
	unsigned int index;

	index = gtk_combo_box_get_active(GTK_COMBO_BOX(c->combo));
	gopt_set_children_visible(o, index);
}

static void gopt_combo_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_combo *c = (struct gopt_combo *) data;

	free(c);
	gtk_widget_destroy(w);
}

static struct gopt_combo *__gopt_new_combo(struct gopt_job_view *gjv,
					   struct fio_option *o,
					   unsigned int idx)
{
	struct gopt_combo *c;
	GtkWidget *label;

	c = malloc(sizeof(*c));
	memset(c, 0, sizeof(*c));

	c->gopt.box = gtk_hbox_new(FALSE, 3);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);

	c->combo = gtk_combo_box_new_text();
	gopt_mark_index(gjv, &c->gopt, idx);
	g_signal_connect(GTK_OBJECT(c->combo), "destroy", G_CALLBACK(gopt_combo_destroy), c);

	gtk_box_pack_start(GTK_BOX(c->gopt.box), c->combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(c->gopt.box), label, FALSE, FALSE, 0);

	return c;
}

static struct gopt *gopt_new_combo_str(struct gopt_job_view *gjv,
				       struct fio_option *o, const char *text,
				       unsigned int idx)
{
	struct gopt_combo *c;
	struct value_pair *vp;
	int i, active = 0;

	c = __gopt_new_combo(gjv, o, idx);

	i = 0;
	vp = &o->posval[0];
	while (vp->ival) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(c->combo), vp->ival);
		if (o->def && !strcmp(vp->ival, o->def))
			active = i;
		if (text && !strcmp(vp->ival, text))
			active = i;
		vp++;
		i++;
	}

	gtk_combo_box_set_active(GTK_COMBO_BOX(c->combo), active);
	c->gopt.sig_handler = g_signal_connect(GTK_OBJECT(c->combo), "changed", G_CALLBACK(gopt_combo_changed), c);
	return &c->gopt;
}

static struct gopt *gopt_new_combo_int(struct gopt_job_view *gjv,
				       struct fio_option *o, unsigned int *ip,
				       unsigned int idx)
{
	struct gopt_combo *c;
	struct value_pair *vp;
	int i, active = 0;

	c = __gopt_new_combo(gjv, o, idx);

	i = 0;
	vp = &o->posval[0];
	while (vp->ival) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(c->combo), vp->ival);
		if (ip && vp->oval == *ip)
			active = i;
		vp++;
		i++;
	}

	gtk_combo_box_set_active(GTK_COMBO_BOX(c->combo), active);
	c->gopt.sig_handler = g_signal_connect(GTK_OBJECT(c->combo), "changed", G_CALLBACK(gopt_combo_changed), c);
	return &c->gopt;
}

static struct gopt *gopt_new_str_multi(struct gopt_job_view *gjv,
				       struct fio_option *o, unsigned int idx)
{
	struct gopt_str_multi *m;
	struct value_pair *vp;
	GtkWidget *frame, *hbox;
	int i;

	m = malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->gopt.box = gtk_hbox_new(FALSE, 3);
	gopt_mark_index(gjv, &m->gopt, idx);

	if (!o->lname)
		frame = gtk_frame_new(o->name);
	else
		frame = gtk_frame_new(o->lname);
	gtk_box_pack_start(GTK_BOX(m->gopt.box), frame, FALSE, FALSE, 3);

	hbox = gtk_hbox_new(FALSE, 3);
	gtk_container_add(GTK_CONTAINER(frame), hbox);

	i = 0;
	vp = &o->posval[0];
	while (vp->ival) {
		m->checks[i] = gtk_check_button_new_with_label(vp->ival);
		gtk_widget_set_tooltip_text(m->checks[i], vp->help);
		gtk_box_pack_start(GTK_BOX(hbox), m->checks[i], FALSE, FALSE, 3);
		vp++;
	}

	return &m->gopt;
}

static void gopt_int_changed(GtkSpinButton *spin, gpointer data)
{
	struct gopt_int *i = (struct gopt_int *) data;
	struct fio_option *o = &fio_options[i->gopt.opt_index];
	GtkAdjustment *adj;
	int value, delta;

	adj = gtk_spin_button_get_adjustment(spin);
	value = gtk_adjustment_get_value(adj);
	delta = value - i->lastval;
	i->lastval = value;

	if (o->inv_opt) {
		struct gopt *b_inv = o->inv_opt->gui_data;
		struct gopt_int *i_inv = container_of(b_inv, struct gopt_int, gopt);
		int cur_val;

		assert(o->type == o->inv_opt->type);

		cur_val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(i_inv->spin));
		cur_val -= delta;
		g_signal_handler_block(G_OBJECT(i_inv->spin), i_inv->gopt.sig_handler);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(i_inv->spin), cur_val);
		g_signal_handler_unblock(G_OBJECT(i_inv->spin), i_inv->gopt.sig_handler);
	}
}

static void gopt_int_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_int *i = (struct gopt_int *) data;

	free(i);
	gtk_widget_destroy(w);
}

static struct gopt_int *__gopt_new_int(struct gopt_job_view *gjv,
				       struct fio_option *o,
				       unsigned long long *p, unsigned int idx)
{
	unsigned long long defval;
	struct gopt_int *i;
	guint maxval, interval;
	GtkWidget *label;

	i = malloc(sizeof(*i));
	memset(i, 0, sizeof(*i));
	i->gopt.box = gtk_hbox_new(FALSE, 3);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);

	maxval = o->maxval;
	if (!maxval)
		maxval = UINT_MAX;

	defval = 0;
	if (p)
		defval = *p;
	else if (o->def) {
		long long val;

		check_str_bytes(o->def, &val, NULL);
		defval = val;
	}

	interval = 1.0;
	if (o->interval)
		interval = o->interval;

	i->spin = gtk_spin_button_new_with_range(o->minval, maxval, interval);
	gopt_mark_index(gjv, &i->gopt, idx);
	gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(i->spin), GTK_UPDATE_IF_VALID);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(i->spin), defval);
	i->lastval = defval;
	i->gopt.sig_handler = g_signal_connect(G_OBJECT(i->spin), "value-changed", G_CALLBACK(gopt_int_changed), i);
	g_signal_connect(G_OBJECT(i->spin), "destroy", G_CALLBACK(gopt_int_destroy), i);

	gtk_box_pack_start(GTK_BOX(i->gopt.box), i->spin, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(i->gopt.box), label, FALSE, FALSE, 0);

	return i;
}

static struct gopt *gopt_new_int(struct gopt_job_view *gjv,
				 struct fio_option *o, unsigned int *ip,
				 unsigned int idx)
{
	unsigned long long ullp;
	struct gopt_int *i;

	if (ip) {
		ullp = *ip;
		i = __gopt_new_int(gjv, o, &ullp, idx);
	} else
		i = __gopt_new_int(gjv, o, NULL, idx);

	return &i->gopt;
}

static struct gopt *gopt_new_ullong(struct gopt_job_view *gjv,
				    struct fio_option *o, unsigned long long *p,
				    unsigned int idx)
{
	struct gopt_int *i;

	i = __gopt_new_int(gjv, o, p, idx);
	return &i->gopt;
}

static void gopt_bool_toggled(GtkToggleButton *button, gpointer data)
{
	struct gopt_bool *b = (struct gopt_bool *) data;
	struct fio_option *o = &fio_options[b->gopt.opt_index];
	gboolean set;

	set = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->check));

	if (o->inv_opt) {
		struct gopt *g_inv = o->inv_opt->gui_data;
		struct gopt_bool *b_inv = container_of(g_inv, struct gopt_bool, gopt);

		assert(o->type == o->inv_opt->type);

		g_signal_handler_block(G_OBJECT(b_inv->check), b_inv->gopt.sig_handler);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_inv->check), !set);
		g_signal_handler_unblock(G_OBJECT(b_inv->check), b_inv->gopt.sig_handler);
	}

	gopt_set_children_visible(o, set);
}

static void gopt_bool_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_bool *b = (struct gopt_bool *) data;

	free(b);
	gtk_widget_destroy(w);
}

static struct gopt *gopt_new_bool(struct gopt_job_view *gjv,
				  struct fio_option *o, unsigned int *val,
				  unsigned int idx)
{
	struct gopt_bool *b;
	GtkWidget *label;
	int defstate = 0;

	b = malloc(sizeof(*b));
	memset(b, 0, sizeof(*b));
	b->gopt.box = gtk_hbox_new(FALSE, 3);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);

	b->check = gtk_check_button_new();
	gopt_mark_index(gjv, &b->gopt, idx);
	if (val)
		defstate = *val;
	else if (o->def && !strcmp(o->def, "1"))
		defstate = 1;

	if (o->neg)
		defstate = !defstate;

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->check), defstate);
	b->gopt.sig_handler = g_signal_connect(G_OBJECT(b->check), "toggled", G_CALLBACK(gopt_bool_toggled), b);
	g_signal_connect(G_OBJECT(b->check), "destroy", G_CALLBACK(gopt_bool_destroy), b);

	gtk_box_pack_start(GTK_BOX(b->gopt.box), b->check, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b->gopt.box), label, FALSE, FALSE, 0);
	return &b->gopt;
}

/*
 * These are paired 0/1 and 2/3. 0/2 are min values, 1/3 are max values.
 * If the max is made smaller than min, adjust min down.
 * If the min is made larger than max, adjust the max.
 */
static void range_value_changed(GtkSpinButton *spin, gpointer data)
{
	struct gopt_range *r = (struct gopt_range *) data;
	int changed = -1, i;
	gint val, mval;

	for (i = 0; i < GOPT_RANGE_SPIN; i++) {
		if (GTK_SPIN_BUTTON(r->spins[i]) == spin) {
			changed = i;
			break;
		}
	}

	assert(changed != -1);

	/*
	 * Min changed
	 */
	if (changed == 0 || changed == 2) {
		GtkWidget *mspin = r->spins[changed + 1];

		val = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->spins[changed]));
		mval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mspin));
		if (val > mval)
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(mspin), val);
	} else {
		GtkWidget *mspin = r->spins[changed - 1];

		val = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->spins[changed]));
		mval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mspin));
		if (val < mval)
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(mspin), val);
	}
}

static void gopt_range_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_range *r = (struct gopt_range *) data;

	free(r);
	gtk_widget_destroy(w);
}

static struct gopt *gopt_new_int_range(struct gopt_job_view *gjv,
				       struct fio_option *o, unsigned int **ip,
				       unsigned int idx)
{
	struct gopt_range *r;
	gint maxval, defval;
	GtkWidget *label;
	guint interval;
	int i;

	r = malloc(sizeof(*r));
	memset(r, 0, sizeof(*r));
	r->gopt.box = gtk_hbox_new(FALSE, 3);
	gopt_mark_index(gjv, &r->gopt, idx);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);

	maxval = o->maxval;
	if (!maxval)
		maxval = INT_MAX;

	defval = 0;
	if (o->def) {
		long long val;

		check_str_bytes(o->def, &val, NULL);
		defval = val;
	}

	interval = 1.0;
	if (o->interval)
		interval = o->interval;

	for (i = 0; i < GOPT_RANGE_SPIN; i++) {
		r->spins[i] = gtk_spin_button_new_with_range(o->minval, maxval, interval);
		gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(r->spins[i]), GTK_UPDATE_IF_VALID);
		if (ip)
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->spins[i]), *ip[i]);
		else
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->spins[i]), defval);

		gtk_box_pack_start(GTK_BOX(r->gopt.box), r->spins[i], FALSE, FALSE, 0);
		g_signal_connect(G_OBJECT(r->spins[i]), "value-changed", G_CALLBACK(range_value_changed), r);
	}

	gtk_box_pack_start(GTK_BOX(r->gopt.box), label, FALSE, FALSE, 0);
	g_signal_connect(G_OBJECT(r->gopt.box), "destroy", G_CALLBACK(gopt_range_destroy), r);
	return &r->gopt;
}

static void gopt_str_val_destroy(GtkWidget *w, gpointer data)
{
	struct gopt_str_val *g = (struct gopt_str_val *) data;

	free(g);
	gtk_widget_destroy(w);
}

static void gopt_str_val_spin_wrapped(GtkSpinButton *spin, gpointer data)
{
	struct gopt_str_val *g = (struct gopt_str_val *) data;
	unsigned int val;
	GtkAdjustment *adj;
	gint index;

	adj = gtk_spin_button_get_adjustment(spin);
	val = gtk_adjustment_get_value(adj);

	/*
	 * Can't rely on exact value, as fast changes increment >= 1
	 */
	if (!val) {
		index = gtk_combo_box_get_active(GTK_COMBO_BOX(g->combo));
		if (index + 1 <= g->maxindex) {
			val = 1;
			gtk_combo_box_set_active(GTK_COMBO_BOX(g->combo), ++index);
		} else
			val = 1023;
		gtk_spin_button_set_value(spin, val);
	} else {
		index = gtk_combo_box_get_active(GTK_COMBO_BOX(g->combo));
		if (index) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(g->combo), --index);
			gtk_spin_button_set_value(spin, 1023);
		} else
			gtk_spin_button_set_value(spin, 0);
	}
}

static struct gopt *gopt_new_str_val(struct gopt_job_view *gjv,
				     struct fio_option *o,
				     unsigned long long *p, unsigned int idx)
{
	struct gopt_str_val *g;
	const gchar *postfix[] = { "B", "KB", "MB", "GB", "PB", "TB", "" };
	GtkWidget *label;
	int i;

	g = malloc(sizeof(*g));
	memset(g, 0, sizeof(*g));
	g->gopt.box = gtk_hbox_new(FALSE, 3);
	if (!o->lname)
		label = gtk_label_new(o->name);
	else
		label = gtk_label_new(o->lname);
	gopt_mark_index(gjv, &g->gopt, idx);

	g->spin = gtk_spin_button_new_with_range(0.0, 1023.0, 1.0);
	gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(g->spin), GTK_UPDATE_IF_VALID);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->spin), 0);
	gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(g->spin), 1);
	gtk_box_pack_start(GTK_BOX(g->gopt.box), g->spin, FALSE, FALSE, 0);
	g_signal_connect(G_OBJECT(g->spin), "wrapped", G_CALLBACK(gopt_str_val_spin_wrapped), g);

	g->combo = gtk_combo_box_new_text();
	i = 0;
	while (strlen(postfix[i])) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(g->combo), postfix[i]);
		i++;
	}
	g->maxindex = i - 1;
	gtk_combo_box_set_active(GTK_COMBO_BOX(g->combo), 0);
	gtk_box_pack_start(GTK_BOX(g->gopt.box), g->combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(g->gopt.box), label, FALSE, FALSE, 3);

	g_signal_connect(G_OBJECT(g->gopt.box), "destroy", G_CALLBACK(gopt_str_val_destroy), g);
	return &g->gopt;
}

static void gopt_add_option(struct gopt_job_view *gjv, GtkWidget *hbox,
			    struct fio_option *o, unsigned int opt_index,
			    struct thread_options *to)
{
	struct gopt *go = NULL;

	switch (o->type) {
	case FIO_OPT_STR_VAL: {
		unsigned long long *ullp = NULL;

		if (o->off1)
			ullp = td_var(to, o->off1);

		go = gopt_new_str_val(gjv, o, ullp, opt_index);
		break;
		}
	case FIO_OPT_STR_VAL_TIME: {
		unsigned long long *ullp = NULL;

		if (o->off1)
			ullp = td_var(to, o->off1);

		go = gopt_new_ullong(gjv, o, ullp, opt_index);
		break;
		}
	case FIO_OPT_INT: {
		unsigned int *ip = NULL;

		if (o->off1)
			ip = td_var(to, o->off1);

		go = gopt_new_int(gjv, o, ip, opt_index);
		break;
		}
	case FIO_OPT_STR_SET:
	case FIO_OPT_BOOL: {
		unsigned int *ip = NULL;

		if (o->off1)
			ip = td_var(to, o->off1);

		go = gopt_new_bool(gjv, o, ip, opt_index);
		break;
		}
	case FIO_OPT_STR: {
		if (o->posval[0].ival) {
			unsigned int *ip = NULL;

			if (o->off1)
				ip = td_var(to, o->off1);

			go = gopt_new_combo_int(gjv, o, ip, opt_index);
		} else {
			/* TODO: usually ->cb, or unsigned int pointer */
			go = gopt_new_str_store(gjv, o, NULL, opt_index);
		}

		break;
		}
	case FIO_OPT_STR_STORE: {
		char *text = NULL;

		if (o->off1) {
			char **p = td_var(to, o->off1);
			text = *p;
		}

		if (!o->posval[0].ival) {
			go = gopt_new_str_store(gjv, o, text, opt_index);
			break;
		}

		go = gopt_new_combo_str(gjv, o, text, opt_index);
		break;
		}
	case FIO_OPT_STR_MULTI:
		go = gopt_new_str_multi(gjv, o, opt_index);
		break;
	case FIO_OPT_RANGE: {
		unsigned int *ip[4] = { td_var(to, o->off1),
					td_var(to, o->off2),
					td_var(to, o->off3),
					td_var(to, o->off4) };

		go = gopt_new_int_range(gjv, o, ip, opt_index);
		break;
		}
	/* still need to handle this one */
	case FIO_OPT_FLOAT_LIST:
		break;
	case FIO_OPT_DEPRECATED:
		break;
	default:
		printf("ignore type %u\n", o->type);
		break;
	}

	if (go) {
		GtkWidget *dest;

		if (o->help)
			gtk_widget_set_tooltip_text(go->box, o->help);

		go->opt_type = o->type;
		o->gui_data = go;

		dest = gopt_get_group_frame(gjv, hbox, o->group);
		if (!dest)
			gtk_box_pack_start(GTK_BOX(hbox), go->box, FALSE, FALSE, 5);
		else
			gtk_box_pack_start(GTK_BOX(dest), go->box, FALSE, FALSE, 5);
	}
}

static void gopt_add_options(struct gopt_job_view *gjv,
			     struct thread_options *to)
{
	GtkWidget *hbox = NULL;
	int i;

	/*
	 * First add all options
	 */
	for (i = 0; fio_options[i].name; i++) {
		struct fio_option *o = &fio_options[i];
		unsigned int mask = o->category;
		struct opt_group *og;

		while ((og = opt_group_from_mask(&mask)) != NULL) {
			GtkWidget *vbox = gjv->vboxes[ffz(~og->mask)];

			hbox = gtk_hbox_new(FALSE, 3);
			gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);
			gopt_add_option(gjv, hbox, o, i, to);
		}
	}
}

static GtkWidget *gopt_add_tab(GtkWidget *notebook, const char *name)
{
	GtkWidget *box, *vbox, *scroll;

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_set_border_width(GTK_CONTAINER(scroll), 5);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	vbox = gtk_vbox_new(FALSE, 3);
	box = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, FALSE, 5);
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), vbox);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new(name));
	return vbox;
}

static GtkWidget *gopt_add_group_tab(GtkWidget *notebook, struct opt_group *og)
{
	return gopt_add_tab(notebook, og->name);
}

static void gopt_add_group_tabs(GtkWidget *notebook, struct gopt_job_view *gjv)
{
	struct opt_group *og;
	unsigned int i;

	i = 0;
	do {
		unsigned int mask = (1U << i);

		og = opt_group_from_mask(&mask);
		if (!og)
			break;
		gjv->vboxes[i] = gopt_add_group_tab(notebook, og);
		i++;
	} while (1);
}

void gopt_get_options_window(GtkWidget *window, struct gfio_client *gc)
{
	GtkWidget *dialog, *notebook, *topnotebook, *vbox;
	struct gfio_client_options *gco;
	struct thread_options *o;
	struct flist_head *entry;
	struct gopt_job_view *gjv;
	FLIST_HEAD(gjv_list);

	/*
	 * Just choose the first item, we need to make each options
	 * entry the main notebook, with the below options view as
	 * a sub-notebook
	 */
	assert(!flist_empty(&gc->o_list));

	dialog = gtk_dialog_new_with_buttons("Fio options",
			GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
			GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT, NULL);

	gtk_widget_set_size_request(GTK_WIDGET(dialog), 1024, 768);

	topnotebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(topnotebook), 1);
	gtk_notebook_popup_enable(GTK_NOTEBOOK(topnotebook));
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), topnotebook, TRUE, TRUE, 5);

	flist_for_each(entry, &gc->o_list) {
		const char *name;

		gco = flist_entry(entry, struct gfio_client_options, list);
		o = &gco->o;
		name = o->name;
		if (!name || !strlen(name))
			name = "Default job";

		vbox = gopt_add_tab(topnotebook, name);

		notebook = gtk_notebook_new();
		gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), 1);
		gtk_notebook_popup_enable(GTK_NOTEBOOK(notebook));
		gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 5);

		gjv = calloc(1, sizeof(*gjv));
		INIT_FLIST_HEAD(&gjv->list);
		flist_add_tail(&gjv->list, &gjv_list);
		gopt_add_group_tabs(notebook, gjv);
		gopt_add_options(gjv, o);
	}

	gtk_widget_show_all(dialog);

	gtk_dialog_run(GTK_DIALOG(dialog));

	gtk_widget_destroy(dialog);

	while (!flist_empty(&gjv_list)) {
		gjv = flist_entry(gjv_list.next, struct gopt_job_view, list);
		flist_del(&gjv->list);
		free(gjv);
	}
}

/*
 * Build n-ary option dependency tree
 */
void gopt_init(void)
{
	int i;

	gopt_dep_tree = g_node_new(NULL);

	for (i = 0; fio_options[i].name; i++) {
		struct fio_option *o = &fio_options[i];
		GNode *node, *nparent;

		/*
		 * Insert node with either the root parent, or an
		 * option parent.
		 */
		node = g_node_new(o);
		nparent = gopt_dep_tree;
		if (o->parent) {
			struct fio_option *parent;

			parent = fio_option_find(o->parent);
			nparent = g_node_find(gopt_dep_tree, G_IN_ORDER, G_TRAVERSE_ALL, parent);
			if (!nparent) {
				log_err("fio: did not find parent %s for opt %s\n", o->name, o->parent);
				nparent = gopt_dep_tree;
			}
		}

		g_node_insert(nparent, -1, node);
	}
}

void gopt_exit(void)
{
	g_node_destroy(gopt_dep_tree);
	gopt_dep_tree = NULL;
}