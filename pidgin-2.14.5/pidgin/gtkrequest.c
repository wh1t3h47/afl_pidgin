/**
 * @file gtkrequest.c GTK+ Request API
 * @ingroup pidgin
 */

/* pidgin
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */
#include "internal.h"
#include "pidgin.h"

#include "debug.h"
#include "prefs.h"
#include "util.h"

#include "gtkimhtml.h"
#include "gtkimhtmltoolbar.h"
#include "gtkrequest.h"
#include "gtkutils.h"
#include "pidginstock.h"
#include "gtkblist.h"
#ifdef USE_VV
#include "media-gst.h"
#ifdef HAVE_GIOUNIX
#include <gio/gunixfdlist.h>
#endif
#endif

#include <gdk/gdkkeysyms.h>

static GtkWidget * create_account_field(PurpleRequestField *field);

typedef struct
{
	PurpleRequestType type;

	void *user_data;
	GtkWidget *dialog;

	GtkWidget *ok_button;

	size_t cb_count;
	GCallback *cbs;

	union
	{
		struct
		{
			GtkWidget *entry;

			gboolean multiline;
			gchar *hint;

		} input;

		struct
		{
			PurpleRequestFields *fields;

		} multifield;

		struct
		{
			gboolean savedialog;
			gchar *name;

		} file;

		struct
		{
#ifdef HAVE_GIOUNIX
			GDBusConnection *dbus_connection;
#endif
			GCancellable *cancellable;
			gchar *session_path;
			guint signal_id;
			guint32 node_id;
			guint portal_session_nr;
			guint portal_request_nr;

		} screenshare;

	} u;

} PidginRequestData;

static void
pidgin_widget_decorate_account(GtkWidget *cont, PurpleAccount *account)
{
	GtkWidget *image;
	GdkPixbuf *pixbuf;
	GtkTooltips *tips;

	if (!account)
		return;

	pixbuf = pidgin_create_prpl_icon(account, PIDGIN_PRPL_ICON_SMALL);
	image = gtk_image_new_from_pixbuf(pixbuf);
	g_object_unref(G_OBJECT(pixbuf));

	tips = gtk_tooltips_new();
	gtk_tooltips_set_tip(tips, image, purple_account_get_username(account), NULL);

	if (GTK_IS_DIALOG(cont)) {
		gtk_box_pack_start(GTK_BOX(GTK_DIALOG(cont)->action_area), image, FALSE, TRUE, 0);
		gtk_box_reorder_child(GTK_BOX(GTK_DIALOG(cont)->action_area), image, 0);
	} else if (GTK_IS_HBOX(cont)) {
		gtk_misc_set_alignment(GTK_MISC(image), 0, 0);
		gtk_box_pack_end(GTK_BOX(cont), image, FALSE, TRUE, 0);
	}
	gtk_widget_show(image);
}

static void
generic_response_start(PidginRequestData *data)
{
	g_return_if_fail(data != NULL);

	/* Tell the user we're doing something. */
	pidgin_set_cursor(GTK_WIDGET(data->dialog), GDK_WATCH);
}

static void
input_response_cb(GtkDialog *dialog, gint id, PidginRequestData *data)
{
	const char *value;
	char *multiline_value = NULL;

	generic_response_start(data);

	if (data->u.input.multiline) {
		GtkTextIter start_iter, end_iter;
		GtkTextBuffer *buffer =
			gtk_text_view_get_buffer(GTK_TEXT_VIEW(data->u.input.entry));

		gtk_text_buffer_get_start_iter(buffer, &start_iter);
		gtk_text_buffer_get_end_iter(buffer, &end_iter);

		if (purple_strequal(data->u.input.hint, "html"))
			multiline_value = gtk_imhtml_get_markup(GTK_IMHTML(data->u.input.entry));
		else
			multiline_value = gtk_text_buffer_get_text(buffer, &start_iter, &end_iter,
										 FALSE);

		value = multiline_value;
	}
	else
		value = gtk_entry_get_text(GTK_ENTRY(data->u.input.entry));

	if (id >= 0 && (gsize)id < data->cb_count && data->cbs[id] != NULL)
		((PurpleRequestInputCb)data->cbs[id])(data->user_data, value);
	else if (data->cbs[1] != NULL)
		((PurpleRequestInputCb)data->cbs[1])(data->user_data, value);

	if (data->u.input.multiline)
		g_free(multiline_value);

	purple_request_close(PURPLE_REQUEST_INPUT, data);
}

static void
action_response_cb(GtkDialog *dialog, gint id, PidginRequestData *data)
{
	generic_response_start(data);

	if (id >= 0 && (gsize)id < data->cb_count && data->cbs[id] != NULL)
		((PurpleRequestActionCb)data->cbs[id])(data->user_data, id);

	purple_request_close(PURPLE_REQUEST_INPUT, data);
}


static void
choice_response_cb(GtkDialog *dialog, gint id, PidginRequestData *data)
{
	GtkWidget *radio = g_object_get_data(G_OBJECT(dialog), "radio");
	GSList *group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(radio));

	generic_response_start(data);

	if (id >= 0 && (gsize)id < data->cb_count && data->cbs[id] != NULL)
		while (group) {
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(group->data))) {
				((PurpleRequestChoiceCb)data->cbs[id])(data->user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(group->data), "choice_id")));
				break;
			}
			group = group->next;
		}
	purple_request_close(PURPLE_REQUEST_INPUT, data);
}

static gboolean
field_string_focus_out_cb(GtkWidget *entry, GdkEventFocus *event,
						  PurpleRequestField *field)
{
	const char *value;

	if (purple_request_field_string_is_multiline(field))
	{
		GtkTextBuffer *buffer;
		GtkTextIter start_iter, end_iter;

		buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));

		gtk_text_buffer_get_start_iter(buffer, &start_iter);
		gtk_text_buffer_get_end_iter(buffer, &end_iter);

		value = gtk_text_buffer_get_text(buffer, &start_iter, &end_iter, FALSE);
	}
	else
		value = gtk_entry_get_text(GTK_ENTRY(entry));

	purple_request_field_string_set_value(field,
			(*value == '\0' ? NULL : value));

	return FALSE;
}

static gboolean
field_int_focus_out_cb(GtkEntry *entry, GdkEventFocus *event,
					   PurpleRequestField *field)
{
	purple_request_field_int_set_value(field,
			atoi(gtk_entry_get_text(entry)));

	return FALSE;
}

static void
field_bool_cb(GtkToggleButton *button, PurpleRequestField *field)
{
	purple_request_field_bool_set_value(field,
			gtk_toggle_button_get_active(button));
}

static void
field_choice_menu_cb(GtkComboBox *menu, PurpleRequestField *field)
{
	purple_request_field_choice_set_value(field,
			gtk_combo_box_get_active(menu));
}

static void
field_choice_option_cb(GtkRadioButton *button, PurpleRequestField *field)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
		purple_request_field_choice_set_value(field,
				(g_slist_length(gtk_radio_button_get_group(button)) -
				 g_slist_index(gtk_radio_button_get_group(button), button)) - 1);
}

static void
field_account_cb(GObject *w, PurpleAccount *account, PurpleRequestField *field)
{
	purple_request_field_account_set_value(field, account);
}

static void
multifield_ok_cb(GtkWidget *button, PidginRequestData *data)
{
	generic_response_start(data);

	if (!GTK_WIDGET_HAS_FOCUS(button))
		gtk_widget_grab_focus(button);

	if (data->cbs[0] != NULL)
		((PurpleRequestFieldsCb)data->cbs[0])(data->user_data,
											data->u.multifield.fields);

	purple_request_close(PURPLE_REQUEST_FIELDS, data);
}

static void
multifield_cancel_cb(GtkWidget *button, PidginRequestData *data)
{
	generic_response_start(data);

	if (data->cbs[1] != NULL)
		((PurpleRequestFieldsCb)data->cbs[1])(data->user_data,
											data->u.multifield.fields);

	purple_request_close(PURPLE_REQUEST_FIELDS, data);
}

static gboolean
destroy_multifield_cb(GtkWidget *dialog, GdkEvent *event,
					  PidginRequestData *data)
{
	multifield_cancel_cb(NULL, data);
	return FALSE;
}


#define STOCK_ITEMIZE(r, l) \
	if (purple_strequal((r), text)) \
		return (l);

static const char *
text_to_stock(const char *text)
{
	STOCK_ITEMIZE(_("Yes"),     GTK_STOCK_YES);
	STOCK_ITEMIZE(_("No"),      GTK_STOCK_NO);
	STOCK_ITEMIZE(_("OK"),      GTK_STOCK_OK);
	STOCK_ITEMIZE(_("Cancel"),  GTK_STOCK_CANCEL);
	STOCK_ITEMIZE(_("Apply"),   GTK_STOCK_APPLY);
	STOCK_ITEMIZE(_("Close"),   GTK_STOCK_CLOSE);
	STOCK_ITEMIZE(_("Delete"),  GTK_STOCK_DELETE);
	STOCK_ITEMIZE(_("Add"),     GTK_STOCK_ADD);
	STOCK_ITEMIZE(_("Remove"),  GTK_STOCK_REMOVE);
	STOCK_ITEMIZE(_("Save"),    GTK_STOCK_SAVE);
	STOCK_ITEMIZE(_("Alias"),   PIDGIN_STOCK_ALIAS);

	return text;
}

static void *
pidgin_request_input(const char *title, const char *primary,
					   const char *secondary, const char *default_value,
					   gboolean multiline, gboolean masked, gchar *hint,
					   const char *ok_text, GCallback ok_cb,
					   const char *cancel_text, GCallback cancel_cb,
					   PurpleAccount *account, const char *who, PurpleConversation *conv,
					   void *user_data)
{
	PidginRequestData *data;
	GtkWidget *dialog;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *entry;
	GtkWidget *img;
	GtkWidget *toolbar;
	char *label_text;
	char *primary_esc, *secondary_esc;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_INPUT;
	data->user_data = user_data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);

	data->cbs[0] = ok_cb;
	data->cbs[1] = cancel_cb;

	/* Create the dialog. */
	dialog = gtk_dialog_new_with_buttons(title ? title : PIDGIN_ALERT_TITLE,
					     NULL, 0,
					     text_to_stock(cancel_text), 1,
					     text_to_stock(ok_text),     0,
					     NULL);
	data->dialog = dialog;

	g_signal_connect(G_OBJECT(dialog), "response",
					 G_CALLBACK(input_response_cb), data);

	/* Setup the dialog */
	gtk_container_set_border_width(GTK_CONTAINER(dialog), PIDGIN_HIG_BORDER/2);
	gtk_container_set_border_width(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER/2);
	if (!multiline)
		gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), 0);
	gtk_box_set_spacing(GTK_BOX(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER);

	/* Setup the main horizontal box */
	hbox = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), hbox);

	/* Dialog icon. */
	img = gtk_image_new_from_stock(PIDGIN_STOCK_DIALOG_QUESTION,
					gtk_icon_size_from_name(PIDGIN_ICON_SIZE_TANGO_HUGE));
	gtk_misc_set_alignment(GTK_MISC(img), 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);

	/* Vertical box */
	vbox = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);

	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

	pidgin_widget_decorate_account(hbox, account);

	/* Descriptive label */
	primary_esc = (primary != NULL) ? g_markup_escape_text(primary, -1) : NULL;
	secondary_esc = (secondary != NULL) ? g_markup_escape_text(secondary, -1) : NULL;
	label_text = g_strdup_printf((primary ? "<span weight=\"bold\" size=\"larger\">"
								 "%s</span>%s%s" : "%s%s%s"),
								 (primary ? primary_esc : ""),
								 ((primary && secondary) ? "\n\n" : ""),
								 (secondary ? secondary_esc : ""));
	g_free(primary_esc);
	g_free(secondary_esc);

	label = gtk_label_new(NULL);

	gtk_label_set_markup(GTK_LABEL(label), label_text);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

	g_free(label_text);

	/* Entry field. */
	data->u.input.multiline = multiline;
	data->u.input.hint = g_strdup(hint);

	gtk_widget_show_all(hbox);

	if (purple_strequal(data->u.input.hint, "html")) {
		GtkWidget *frame;

		/* imhtml */
		frame = pidgin_create_imhtml(TRUE, &entry, &toolbar, NULL);
		gtk_widget_set_size_request(entry, 320, 130);
		gtk_widget_set_name(entry, "pidgin_request_imhtml");
		if (default_value != NULL)
			gtk_imhtml_append_text(GTK_IMHTML(entry), default_value, GTK_IMHTML_NO_SCROLL);
		gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);
		gtk_widget_show(frame);

		gtk_imhtml_set_return_inserts_newline(GTK_IMHTML(entry));
	}
	else {
		if (multiline) {
			/* GtkTextView */
			entry = gtk_text_view_new();
			gtk_text_view_set_editable(GTK_TEXT_VIEW(entry), TRUE);

			if (default_value != NULL) {
				GtkTextBuffer *buffer;

				buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
				gtk_text_buffer_set_text(buffer, default_value, -1);
			}

			gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(entry), GTK_WRAP_WORD_CHAR);

			if (purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/conversations/spellcheck"))
				pidgin_setup_gtkspell(GTK_TEXT_VIEW(entry));

			gtk_box_pack_start(GTK_BOX(vbox),
				pidgin_make_scrollable(entry, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS, GTK_SHADOW_IN, 320, 130),
				TRUE, TRUE, 0);
		}
		else {
			entry = gtk_entry_new();

			gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

			gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

			if (default_value != NULL)
				gtk_entry_set_text(GTK_ENTRY(entry), default_value);

			if (masked)
			{
				gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
#if !GTK_CHECK_VERSION(2,16,0)
				if (gtk_entry_get_invisible_char(GTK_ENTRY(entry)) == '*')
					gtk_entry_set_invisible_char(GTK_ENTRY(entry), PIDGIN_INVISIBLE_CHAR);
#endif /* Less than GTK+ 2.16 */
			}
		}
		gtk_widget_show_all(vbox);
	}

	pidgin_set_accessible_label (entry, label);
	data->u.input.entry = entry;

	pidgin_auto_parent_window(dialog);

	/* Show everything. */
	gtk_widget_show(dialog);

	return data;
}

static void *
pidgin_request_choice(const char *title, const char *primary,
			const char *secondary, int default_value,
			const char *ok_text, GCallback ok_cb,
			const char *cancel_text, GCallback cancel_cb,
			PurpleAccount *account, const char *who, PurpleConversation *conv,
			void *user_data, va_list args)
{
	PidginRequestData *data;
	GtkWidget *dialog;
	GtkWidget *vbox, *vbox2;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *img;
	GtkWidget *radio = NULL;
	char *label_text;
	char *radio_text;
	char *primary_esc, *secondary_esc;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_ACTION;
	data->user_data = user_data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;

	/* Create the dialog. */
	data->dialog = dialog = gtk_dialog_new();

	if (title != NULL)
		gtk_window_set_title(GTK_WINDOW(dialog), title);
#ifdef _WIN32
		gtk_window_set_title(GTK_WINDOW(dialog), PIDGIN_ALERT_TITLE);
#endif

	gtk_dialog_add_button(GTK_DIALOG(dialog),
			      text_to_stock(cancel_text), 0);

	gtk_dialog_add_button(GTK_DIALOG(dialog),
			      text_to_stock(ok_text), 1);

	g_signal_connect(G_OBJECT(dialog), "response",
			 G_CALLBACK(choice_response_cb), data);

	/* Setup the dialog */
	gtk_container_set_border_width(GTK_CONTAINER(dialog), PIDGIN_HIG_BORDER/2);
	gtk_container_set_border_width(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER/2);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);
	gtk_box_set_spacing(GTK_BOX(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER);

	/* Setup the main horizontal box */
	hbox = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), hbox);

	/* Dialog icon. */
	img = gtk_image_new_from_stock(PIDGIN_STOCK_DIALOG_QUESTION,
				       gtk_icon_size_from_name(PIDGIN_ICON_SIZE_TANGO_HUGE));
	gtk_misc_set_alignment(GTK_MISC(img), 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);

	pidgin_widget_decorate_account(hbox, account);

	/* Vertical box */
	vbox = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);

	/* Descriptive label */
	primary_esc = (primary != NULL) ? g_markup_escape_text(primary, -1) : NULL;
	secondary_esc = (secondary != NULL) ? g_markup_escape_text(secondary, -1) : NULL;
	label_text = g_strdup_printf((primary ? "<span weight=\"bold\" size=\"larger\">"
				      "%s</span>%s%s" : "%s%s%s"),
				     (primary ? primary_esc : ""),
				     ((primary && secondary) ? "\n\n" : ""),
				     (secondary ? secondary_esc : ""));
	g_free(primary_esc);
	g_free(secondary_esc);

	label = gtk_label_new(NULL);

	gtk_label_set_markup(GTK_LABEL(label), label_text);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

	g_free(label_text);

	vbox2 = gtk_vbox_new(FALSE, PIDGIN_HIG_BOX_SPACE);
	gtk_box_pack_start(GTK_BOX(vbox), vbox2, FALSE, FALSE, 0);
	while ((radio_text = va_arg(args, char*))) {
		       int resp = va_arg(args, int);
		       radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio), radio_text);
		       gtk_box_pack_start(GTK_BOX(vbox2), radio, FALSE, FALSE, 0);
		       g_object_set_data(G_OBJECT(radio), "choice_id", GINT_TO_POINTER(resp));
		       if (resp == default_value)
			       gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);
	}

	g_object_set_data(G_OBJECT(dialog), "radio", radio);

	/* Show everything. */
	pidgin_auto_parent_window(dialog);

	gtk_widget_show_all(dialog);

	return data;
}

static void *
pidgin_request_action_with_icon(const char *title, const char *primary,
						const char *secondary, int default_action,
					    PurpleAccount *account, const char *who,
						PurpleConversation *conv, gconstpointer icon_data,
						gsize icon_size,
						void *user_data, size_t action_count, va_list actions)
{
	PidginRequestData *data;
	GtkWidget *dialog;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *img = NULL;
	void **buttons;
	char *label_text;
	char *primary_esc, *secondary_esc;
	gsize i;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_ACTION;
	data->user_data = user_data;

	data->cb_count = action_count;
	data->cbs = g_new0(GCallback, action_count);

	/* Reverse the buttons */
	buttons = g_new0(void *, action_count * 2);

	for (i = 0; i < action_count * 2; i += 2) {
		buttons[(action_count * 2) - i - 2] = va_arg(actions, char *);
		buttons[(action_count * 2) - i - 1] = va_arg(actions, GCallback);
	}

	/* Create the dialog. */
	data->dialog = dialog = gtk_dialog_new();

	gtk_window_set_deletable(GTK_WINDOW(data->dialog), FALSE);

	if (title != NULL)
		gtk_window_set_title(GTK_WINDOW(dialog), title);
#ifdef _WIN32
	else
		gtk_window_set_title(GTK_WINDOW(dialog), PIDGIN_ALERT_TITLE);
#endif

	for (i = 0; i < action_count; i++) {
		gtk_dialog_add_button(GTK_DIALOG(dialog),
							  text_to_stock(buttons[2 * i]), i);

		data->cbs[i] = buttons[2 * i + 1];
	}

	g_free(buttons);

	g_signal_connect(G_OBJECT(dialog), "response",
					 G_CALLBACK(action_response_cb), data);

	/* Setup the dialog */
	gtk_container_set_border_width(GTK_CONTAINER(dialog), PIDGIN_HIG_BORDER/2);
	gtk_container_set_border_width(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER/2);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);
	gtk_box_set_spacing(GTK_BOX(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER);

	/* Setup the main horizontal box */
	hbox = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), hbox);

	/* Dialog icon. */
	if (icon_data) {
		GdkPixbuf *pixbuf = pidgin_pixbuf_from_data(icon_data, icon_size);
		if (pixbuf) {
			/* scale the image if it is too large */
			int width = gdk_pixbuf_get_width(pixbuf);
			int height = gdk_pixbuf_get_height(pixbuf);
			if (width > 128 || height > 128) {
				int scaled_width = width > height ? 128 : (128 * width) / height;
				int scaled_height = height > width ? 128 : (128 * height) / width;
				GdkPixbuf *scaled =
						gdk_pixbuf_scale_simple(pixbuf, scaled_width, scaled_height,
						    GDK_INTERP_BILINEAR);

				purple_debug_info("pidgin",
				    "dialog icon was too large, scaled it down\n");
				if (scaled) {
					g_object_unref(pixbuf);
					pixbuf = scaled;
				}
			}
			img = gtk_image_new_from_pixbuf(pixbuf);
			g_object_unref(pixbuf);
		} else {
			purple_debug_info("pidgin", "failed to parse dialog icon\n");
		}
	}

	if (!img) {
		img = gtk_image_new_from_stock(PIDGIN_STOCK_DIALOG_QUESTION,
				       gtk_icon_size_from_name(PIDGIN_ICON_SIZE_TANGO_HUGE));
	}
	gtk_misc_set_alignment(GTK_MISC(img), 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);

	/* Vertical box */
	vbox = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);

	pidgin_widget_decorate_account(hbox, account);

	/* Descriptive label */
	primary_esc = (primary != NULL) ? g_markup_escape_text(primary, -1) : NULL;
	secondary_esc = (secondary != NULL) ? g_markup_escape_text(secondary, -1) : NULL;
	label_text = g_strdup_printf((primary ? "<span weight=\"bold\" size=\"larger\">"
								 "%s</span>%s%s" : "%s%s%s"),
								 (primary ? primary_esc : ""),
								 ((primary && secondary) ? "\n\n" : ""),
								 (secondary ? secondary_esc : ""));
	g_free(primary_esc);
	g_free(secondary_esc);

	label = gtk_label_new(NULL);

	gtk_label_set_markup(GTK_LABEL(label), label_text);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

	g_free(label_text);


	if (default_action == PURPLE_DEFAULT_ACTION_NONE) {
		GTK_WIDGET_SET_FLAGS(img, GTK_CAN_DEFAULT);
		GTK_WIDGET_SET_FLAGS(img, GTK_CAN_FOCUS);
		gtk_widget_grab_focus(img);
		gtk_widget_grab_default(img);
	} else
		/*
		 * Need to invert the default_action number because the
		 * buttons are added to the dialog in reverse order.
		 */
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), action_count - 1 - default_action);

	/* Show everything. */
	pidgin_auto_parent_window(dialog);

	gtk_widget_show_all(dialog);

	return data;
}

static void *
pidgin_request_action(const char *title, const char *primary,
						const char *secondary, int default_action,
					    PurpleAccount *account, const char *who, PurpleConversation *conv,
						void *user_data, size_t action_count, va_list actions)
{
	return pidgin_request_action_with_icon(title, primary, secondary,
		default_action, account, who, conv, NULL, 0, user_data, action_count,
		actions);
}

static void
req_entry_field_changed_cb(GtkWidget *entry, PurpleRequestField *field)
{
	PurpleRequestFieldGroup *group;
	PidginRequestData *req_data;

	if (purple_request_field_string_is_multiline(field))
	{
		char *text;
		GtkTextIter start_iter, end_iter;

		gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(entry), &start_iter);
		gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(entry), &end_iter);

		text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(entry), &start_iter, &end_iter, FALSE);
		purple_request_field_string_set_value(field, (!text || !*text) ? NULL : text);
		g_free(text);
	}
	else
	{
		const char *text = NULL;
		text = gtk_entry_get_text(GTK_ENTRY(entry));
		purple_request_field_string_set_value(field, (*text == '\0') ? NULL : text);
	}

	group = purple_request_field_get_group(field);
	req_data = (PidginRequestData *)group->fields_list->ui_data;

	gtk_widget_set_sensitive(req_data->ok_button,
		purple_request_fields_all_required_filled(group->fields_list));
}

static void
setup_entry_field(GtkWidget *entry, PurpleRequestField *field)
{
	const char *type_hint;

	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

	if (purple_request_field_is_required(field))
	{
		g_signal_connect(G_OBJECT(entry), "changed",
						 G_CALLBACK(req_entry_field_changed_cb), field);
	}

	if ((type_hint = purple_request_field_get_type_hint(field)) != NULL)
	{
		if (purple_str_has_prefix(type_hint, "screenname"))
		{
			GtkWidget *optmenu = NULL;
			PurpleRequestFieldGroup *group = purple_request_field_get_group(field);
			GList *fields = group->fields;

			/* Ensure the account option menu is created (if the widget hasn't
			 * been initialized already) for username auto-completion. */
			while (fields)
			{
				PurpleRequestField *fld = fields->data;
				fields = fields->next;

				if (purple_request_field_get_type(fld) == PURPLE_REQUEST_FIELD_ACCOUNT &&
						purple_request_field_is_visible(fld))
				{
					const char *type_hint = purple_request_field_get_type_hint(fld);
					if (purple_strequal(type_hint, "account"))
					{
						optmenu = GTK_WIDGET(purple_request_field_get_ui_data(fld));
						if (optmenu == NULL) {
							optmenu = GTK_WIDGET(create_account_field(fld));
							purple_request_field_set_ui_data(fld, optmenu);
						}
						break;
					}
				}
			}
			pidgin_setup_screenname_autocomplete_with_filter(entry, optmenu, pidgin_screenname_autocomplete_default_filter, GINT_TO_POINTER(purple_strequal(type_hint, "screenname-all")));
		}
	}
}

static GtkWidget *
create_string_field(PurpleRequestField *field)
{
	const char *value;
	GtkWidget *widget;

	value = purple_request_field_string_get_default_value(field);

	if (purple_request_field_string_is_multiline(field))
	{
		GtkWidget *textview;

		textview = gtk_text_view_new();
		gtk_text_view_set_editable(GTK_TEXT_VIEW(textview),
								   TRUE);
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview),
									GTK_WRAP_WORD_CHAR);

		if (purple_prefs_get_bool(PIDGIN_PREFS_ROOT "/conversations/spellcheck"))
			pidgin_setup_gtkspell(GTK_TEXT_VIEW(textview));

		gtk_widget_show(textview);

		if (value != NULL)
		{
			GtkTextBuffer *buffer;

			buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));

			gtk_text_buffer_set_text(buffer, value, -1);
		}

		gtk_text_view_set_editable(GTK_TEXT_VIEW(textview),
			purple_request_field_string_is_editable(field));

		g_signal_connect(G_OBJECT(textview), "focus-out-event",
						 G_CALLBACK(field_string_focus_out_cb), field);

	    if (purple_request_field_is_required(field))
	    {
			GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
			g_signal_connect(G_OBJECT(buffer), "changed",
							 G_CALLBACK(req_entry_field_changed_cb), field);
	    }

		widget = pidgin_make_scrollable(textview, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS, GTK_SHADOW_IN, -1, 75);
	}
	else
	{
		widget = gtk_entry_new();

		setup_entry_field(widget, field);

		if (value != NULL)
			gtk_entry_set_text(GTK_ENTRY(widget), value);

		if (purple_request_field_string_is_masked(field))
		{
			gtk_entry_set_visibility(GTK_ENTRY(widget), FALSE);
#if !GTK_CHECK_VERSION(2,16,0)
			if (gtk_entry_get_invisible_char(GTK_ENTRY(widget)) == '*')
				gtk_entry_set_invisible_char(GTK_ENTRY(widget),	PIDGIN_INVISIBLE_CHAR);
#endif /* Less than GTK+ 2.16 */
		}

		gtk_editable_set_editable(GTK_EDITABLE(widget),
			purple_request_field_string_is_editable(field));

		g_signal_connect(G_OBJECT(widget), "focus-out-event",
						 G_CALLBACK(field_string_focus_out_cb), field);
	}

	return widget;
}

static GtkWidget *
create_int_field(PurpleRequestField *field)
{
	int value;
	GtkWidget *widget;

	widget = gtk_entry_new();

	setup_entry_field(widget, field);

	value = purple_request_field_int_get_default_value(field);

	if (value != 0)
	{
		char buf[32];

		g_snprintf(buf, sizeof(buf), "%d", value);

		gtk_entry_set_text(GTK_ENTRY(widget), buf);
	}

	g_signal_connect(G_OBJECT(widget), "focus-out-event",
					 G_CALLBACK(field_int_focus_out_cb), field);

	return widget;
}

static GtkWidget *
create_bool_field(PurpleRequestField *field)
{
	GtkWidget *widget;

	widget = gtk_check_button_new_with_label(
		purple_request_field_get_label(field));

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
		purple_request_field_bool_get_default_value(field));

	g_signal_connect(G_OBJECT(widget), "toggled",
					 G_CALLBACK(field_bool_cb), field);

	return widget;
}

static GtkWidget *
create_choice_field(PurpleRequestField *field)
{
	GtkWidget *widget;
	GList *labels = purple_request_field_choice_get_labels(field);
	int num_labels = g_list_length(labels);
	GList *l;

	if (num_labels > 5)
	{
		widget = gtk_combo_box_new_text();

		for (l = labels; l != NULL; l = l->next)
		{
			const char *text = l->data;
			gtk_combo_box_append_text(GTK_COMBO_BOX(widget), text);
		}

		gtk_combo_box_set_active(GTK_COMBO_BOX(widget),
						purple_request_field_choice_get_default_value(field));

		g_signal_connect(G_OBJECT(widget), "changed",
						 G_CALLBACK(field_choice_menu_cb), field);
	}
	else
	{
		GtkWidget *box;
		GtkWidget *first_radio = NULL;
		GtkWidget *radio;
		gint i;

		if (num_labels == 2)
			box = gtk_hbox_new(FALSE, PIDGIN_HIG_BOX_SPACE);
		else
			box = gtk_vbox_new(FALSE, 0);

		widget = box;

		for (l = labels, i = 0; l != NULL; l = l->next, i++)
		{
			const char *text = l->data;

			radio = gtk_radio_button_new_with_label_from_widget(
				GTK_RADIO_BUTTON(first_radio), text);

			if (first_radio == NULL)
				first_radio = radio;

			if (i == purple_request_field_choice_get_default_value(field))
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

			gtk_box_pack_start(GTK_BOX(box), radio, TRUE, TRUE, 0);
			gtk_widget_show(radio);

			g_signal_connect(G_OBJECT(radio), "toggled",
							 G_CALLBACK(field_choice_option_cb), field);
		}
	}

	return widget;
}

static GtkWidget *
create_image_field(PurpleRequestField *field)
{
	GtkWidget *widget;
	GdkPixbuf *buf, *scale;

	buf = pidgin_pixbuf_from_data(
			(const guchar *)purple_request_field_image_get_buffer(field),
			purple_request_field_image_get_size(field));

	scale = gdk_pixbuf_scale_simple(buf,
			purple_request_field_image_get_scale_x(field) * gdk_pixbuf_get_width(buf),
			purple_request_field_image_get_scale_y(field) * gdk_pixbuf_get_height(buf),
			GDK_INTERP_BILINEAR);
	widget = gtk_image_new_from_pixbuf(scale);
	g_object_unref(G_OBJECT(buf));
	g_object_unref(G_OBJECT(scale));

	return widget;
}

static GtkWidget *
create_account_field(PurpleRequestField *field)
{
	GtkWidget *widget;

	widget = pidgin_account_option_menu_new(
		purple_request_field_account_get_default_value(field),
		purple_request_field_account_get_show_all(field),
		G_CALLBACK(field_account_cb),
		purple_request_field_account_get_filter(field),
		field);

	return widget;
}

static void
select_field_list_item(GtkTreeModel *model, GtkTreePath *path,
					   GtkTreeIter *iter, gpointer data)
{
	PurpleRequestField *field = (PurpleRequestField *)data;
	char *text;

	gtk_tree_model_get(model, iter, 1, &text, -1);

	purple_request_field_list_add_selected(field, text);
	g_free(text);
}

static void
list_field_select_changed_cb(GtkTreeSelection *sel, PurpleRequestField *field)
{
	purple_request_field_list_clear_selected(field);

	gtk_tree_selection_selected_foreach(sel, select_field_list_item, field);
}

static GtkWidget *
create_list_field(PurpleRequestField *field)
{
	GtkWidget *treeview;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeSelection *sel;
	GtkTreeViewColumn *column;
	GtkTreeIter iter;
	GList *l;
	GList *icons = NULL;

	icons = purple_request_field_list_get_icons(field);


	/* Create the list store */
	if (icons)
		store = gtk_list_store_new(3, G_TYPE_POINTER, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	else
		store = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_STRING);

	/* Create the tree view */
	treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(G_OBJECT(store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeview), FALSE);

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

	if (purple_request_field_list_get_multi_select(field))
		gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);

	column = gtk_tree_view_column_new();
	gtk_tree_view_insert_column(GTK_TREE_VIEW(treeview), column, -1);

	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(column, renderer, TRUE);
	gtk_tree_view_column_add_attribute(column, renderer, "text", 1);

	if (icons)
	{
		renderer = gtk_cell_renderer_pixbuf_new();
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_add_attribute(column, renderer, "pixbuf", 2);

		gtk_widget_set_size_request(treeview, 200, 400);
	}

	for (l = purple_request_field_list_get_items(field); l != NULL; l = l->next)
	{
		const char *text = (const char *)l->data;

		gtk_list_store_append(store, &iter);

		if (icons)
		{
			const char *icon_path = (const char *)icons->data;
			GdkPixbuf* pixbuf = NULL;

			if (icon_path)
				pixbuf = pidgin_pixbuf_new_from_file(icon_path);

			gtk_list_store_set(store, &iter,
						   0, purple_request_field_list_get_data(field, text),
						   1, text,
						   2, pixbuf,
						   -1);
			icons = icons->next;
		}
		else
			gtk_list_store_set(store, &iter,
						   0, purple_request_field_list_get_data(field, text),
						   1, text,
						   -1);

		if (purple_request_field_list_is_selected(field, text))
			gtk_tree_selection_select_iter(sel, &iter);
	}

	/*
	 * We only want to catch changes made by the user, so it's important
	 * that we wait until after the list is created to connect this
	 * handler.  If we connect the handler before the loop above and
	 * there are multiple items selected, then selecting the first iter
	 * in the tree causes list_field_select_changed_cb to be triggered
	 * which clears out the rest of the list of selected items.
	 */
	g_signal_connect(G_OBJECT(sel), "changed",
					 G_CALLBACK(list_field_select_changed_cb), field);

	gtk_widget_show(treeview);

	return pidgin_make_scrollable(treeview, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC, GTK_SHADOW_IN, -1, -1);
}

static void *
pidgin_request_fields(const char *title, const char *primary,
						const char *secondary, PurpleRequestFields *fields,
						const char *ok_text, GCallback ok_cb,
						const char *cancel_text, GCallback cancel_cb,
					    PurpleAccount *account, const char *who, PurpleConversation *conv,
						void *user_data)
{
	PidginRequestData *data;
	GtkWidget *win;
	GtkWidget *vbox;
	GtkWidget *vbox2;
	GtkWidget *hbox;
	GtkWidget *frame;
	GtkWidget *label;
	GtkWidget *table;
	GtkWidget *button;
	GtkWidget *img;
	GtkSizeGroup *sg;
	GList *gl, *fl;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	char *label_text;
	char *primary_esc, *secondary_esc;
	int total_fields = 0;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_FIELDS;
	data->user_data = user_data;
	data->u.multifield.fields = fields;

	fields->ui_data = data;

	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);

	data->cbs[0] = ok_cb;
	data->cbs[1] = cancel_cb;


#ifdef _WIN32
	data->dialog = win = pidgin_create_dialog(PIDGIN_ALERT_TITLE, PIDGIN_HIG_BORDER, "multifield", TRUE) ;
#else /* !_WIN32 */
	data->dialog = win = pidgin_create_dialog(title, PIDGIN_HIG_BORDER, "multifield", TRUE) ;
#endif /* _WIN32 */

	g_signal_connect(G_OBJECT(win), "delete_event",
					 G_CALLBACK(destroy_multifield_cb), data);

	/* Setup the main horizontal box */
	hbox = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_container_add(GTK_CONTAINER(pidgin_dialog_get_vbox(GTK_DIALOG(win))), hbox);
	gtk_widget_show(hbox);

	/* Dialog icon. */
	img = gtk_image_new_from_stock(PIDGIN_STOCK_DIALOG_QUESTION,
					gtk_icon_size_from_name(PIDGIN_ICON_SIZE_TANGO_HUGE));
	gtk_misc_set_alignment(GTK_MISC(img), 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);
	gtk_widget_show(img);

	/* Cancel button */
	button = pidgin_dialog_add_button(GTK_DIALOG(win), text_to_stock(cancel_text), G_CALLBACK(multifield_cancel_cb), data);
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);

	/* OK button */
	button = pidgin_dialog_add_button(GTK_DIALOG(win), text_to_stock(ok_text), G_CALLBACK(multifield_ok_cb), data);
	data->ok_button = button;
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_window_set_default(GTK_WINDOW(win), button);

	pidgin_widget_decorate_account(hbox, account);

	/* Setup the vbox */
	vbox = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
	gtk_widget_show(vbox);

	sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	if(primary) {
		primary_esc = g_markup_escape_text(primary, -1);
		label_text = g_strdup_printf(
				"<span weight=\"bold\" size=\"larger\">%s</span>", primary_esc);
		g_free(primary_esc);
		label = gtk_label_new(NULL);

		gtk_label_set_markup(GTK_LABEL(label), label_text);
		gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
		gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
		gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
		gtk_widget_show(label);
		g_free(label_text);
	}

	for (gl = purple_request_fields_get_groups(fields); gl != NULL;
			gl = gl->next)
		total_fields += g_list_length(purple_request_field_group_get_fields(gl->data));

	if(total_fields > 9) {
		GtkWidget *hbox_for_spacing, *vbox_for_spacing;

		hbox_for_spacing = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
		gtk_box_pack_start(GTK_BOX(vbox),
			pidgin_make_scrollable(hbox_for_spacing, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC, GTK_SHADOW_NONE, -1, 200),
			TRUE, TRUE, 0);
		gtk_widget_show(hbox_for_spacing);

		vbox_for_spacing = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
		gtk_box_pack_start(GTK_BOX(hbox_for_spacing),
				vbox_for_spacing, TRUE, TRUE, PIDGIN_HIG_BOX_SPACE);
		gtk_widget_show(vbox_for_spacing);

		vbox2 = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
		gtk_box_pack_start(GTK_BOX(vbox_for_spacing),
				vbox2, TRUE, TRUE, PIDGIN_HIG_BOX_SPACE);
		gtk_widget_show(vbox2);
	} else {
		vbox2 = vbox;
	}

	if (secondary) {
		secondary_esc = g_markup_escape_text(secondary, -1);
		label = gtk_label_new(NULL);

		gtk_label_set_markup(GTK_LABEL(label), secondary_esc);
		g_free(secondary_esc);
		gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
		gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
		gtk_box_pack_start(GTK_BOX(vbox2), label, TRUE, TRUE, 0);
		gtk_widget_show(label);
	}

	for (gl = purple_request_fields_get_groups(fields);
		 gl != NULL;
		 gl = gl->next)
	{
		GList *field_list;
		size_t field_count = 0;
		size_t cols = 1;
		size_t rows;
#if 0
		size_t col_num;
#endif
		size_t row_num = 0;

		group      = gl->data;
		field_list = purple_request_field_group_get_fields(group);

		if (purple_request_field_group_get_title(group) != NULL)
		{
			frame = pidgin_make_frame(vbox2,
				purple_request_field_group_get_title(group));
		}
		else
			frame = vbox2;

		field_count = g_list_length(field_list);
#if 0
		if (field_count > 9)
		{
			rows = field_count / 2;
			cols++;
		}
		else
#endif
			rows = field_count;

#if 0
		col_num = 0;
#endif

		for (fl = field_list; fl != NULL; fl = fl->next)
		{
			PurpleRequestFieldType type;

			field = (PurpleRequestField *)fl->data;

			type = purple_request_field_get_type(field);

			if (type == PURPLE_REQUEST_FIELD_LABEL)
			{
#if 0
				if (col_num > 0)
					rows++;
#endif

				rows++;
			}
			else if ((type == PURPLE_REQUEST_FIELD_LIST) ||
				 (type == PURPLE_REQUEST_FIELD_STRING &&
				  purple_request_field_string_is_multiline(field)))
			{
#if 0
				if (col_num > 0)
					rows++;
#endif

				rows += 2;
			}

#if 0
			col_num++;

			if (col_num >= cols)
				col_num = 0;
#endif
		}

		table = gtk_table_new(rows, 2 * cols, FALSE);
		gtk_table_set_row_spacings(GTK_TABLE(table), PIDGIN_HIG_BOX_SPACE);
		gtk_table_set_col_spacings(GTK_TABLE(table), PIDGIN_HIG_BOX_SPACE);

		gtk_container_add(GTK_CONTAINER(frame), table);
		gtk_widget_show(table);

		for (row_num = 0, fl = field_list;
			 row_num < rows && fl != NULL;
			 row_num++)
		{
#if 0
			for (col_num = 0;
				 col_num < cols && fl != NULL;
				 col_num++, fl = fl->next)
#else
			gboolean dummy_counter = TRUE;
			/* it's the same as loop above */
			for (; dummy_counter && fl != NULL; dummy_counter = FALSE, fl = fl->next)
#endif
			{
#if 0
				size_t col_offset = col_num * 2;
#else
				size_t col_offset = 0;
#endif
				PurpleRequestFieldType type;
				GtkWidget *widget = NULL;
				const char *field_label;

				label = NULL;
				field = fl->data;

				if (!purple_request_field_is_visible(field)) {
#if 0
					col_num--;
#endif
					continue;
				}

				type = purple_request_field_get_type(field);
				field_label = purple_request_field_get_label(field);

				if (type != PURPLE_REQUEST_FIELD_BOOLEAN && field_label)
				{
					char *text = NULL;

					if (field_label[strlen(field_label) - 1] != ':')
						text = g_strdup_printf("%s:", field_label);

					label = gtk_label_new(NULL);
					gtk_label_set_markup_with_mnemonic(GTK_LABEL(label), text ? text : field_label);
					g_free(text);

					gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);

					gtk_size_group_add_widget(sg, label);

					if (type == PURPLE_REQUEST_FIELD_LABEL ||
					    type == PURPLE_REQUEST_FIELD_LIST ||
						(type == PURPLE_REQUEST_FIELD_STRING &&
						 purple_request_field_string_is_multiline(field)))
					{
#if 0
						if(col_num > 0)
							row_num++;
#endif

						gtk_table_attach_defaults(GTK_TABLE(table), label,
												  0, 2 * cols,
												  row_num, row_num + 1);

						row_num++;
#if 0
						col_num=cols;
#endif
					}
					else
					{
						gtk_table_attach_defaults(GTK_TABLE(table), label,
												  col_offset, col_offset + 1,
												  row_num, row_num + 1);
					}

					gtk_widget_show(label);
				}

				widget = GTK_WIDGET(purple_request_field_get_ui_data(field));
				if (widget == NULL)
				{
					if (type == PURPLE_REQUEST_FIELD_STRING)
						widget = create_string_field(field);
					else if (type == PURPLE_REQUEST_FIELD_INTEGER)
						widget = create_int_field(field);
					else if (type == PURPLE_REQUEST_FIELD_BOOLEAN)
						widget = create_bool_field(field);
					else if (type == PURPLE_REQUEST_FIELD_CHOICE)
						widget = create_choice_field(field);
					else if (type == PURPLE_REQUEST_FIELD_LIST)
						widget = create_list_field(field);
					else if (type == PURPLE_REQUEST_FIELD_IMAGE)
						widget = create_image_field(field);
					else if (type == PURPLE_REQUEST_FIELD_ACCOUNT)
						widget = create_account_field(field);
					else
						continue;
				}

				if (label)
					gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);

				if (type == PURPLE_REQUEST_FIELD_STRING &&
					purple_request_field_string_is_multiline(field))
				{
					gtk_table_attach(GTK_TABLE(table), widget,
									 0, 2 * cols,
									 row_num, row_num + 1,
									 GTK_FILL | GTK_EXPAND,
									 GTK_FILL | GTK_EXPAND,
									 5, 0);
				}
				else if (type == PURPLE_REQUEST_FIELD_LIST)
				{
									gtk_table_attach(GTK_TABLE(table), widget,
									0, 2 * cols,
									row_num, row_num + 1,
									GTK_FILL | GTK_EXPAND,
									GTK_FILL | GTK_EXPAND,
									5, 0);
				}
				else if (type == PURPLE_REQUEST_FIELD_BOOLEAN)
				{
					gtk_table_attach(GTK_TABLE(table), widget,
									 col_offset, col_offset + 1,
									 row_num, row_num + 1,
									 GTK_FILL | GTK_EXPAND,
									 GTK_FILL | GTK_EXPAND,
									 5, 0);
				}
				else
				{
					gtk_table_attach(GTK_TABLE(table), widget,
							 		 1, 2 * cols,
									 row_num, row_num + 1,
									 GTK_FILL | GTK_EXPAND,
									 GTK_FILL | GTK_EXPAND,
									 5, 0);
				}

				gtk_widget_show(widget);

				purple_request_field_set_ui_data(field, widget);
			}
		}
	}

	g_object_unref(sg);

	if (!purple_request_fields_all_required_filled(fields))
		gtk_widget_set_sensitive(data->ok_button, FALSE);

	pidgin_auto_parent_window(win);

	gtk_widget_show(win);

	return data;
}

static void
file_yes_no_cb(PidginRequestData *data, gint id)
{
	/* Only call the callback if yes was selected, otherwise the request
	 * (eg. file transfer) will be cancelled, then when a new filename is chosen
	 * things go BOOM */
	if (id == 1) {
		if (data->cbs[1] != NULL)
			((PurpleRequestFileCb)data->cbs[1])(data->user_data, data->u.file.name);
		purple_request_close(data->type, data);
	} else {
		pidgin_clear_cursor(GTK_WIDGET(data->dialog));
	}
}

static void
file_ok_check_if_exists_cb(GtkWidget *widget, gint response, PidginRequestData *data)
{
	gchar *current_folder;

	generic_response_start(data);

	if (response != GTK_RESPONSE_ACCEPT) {
		if (data->cbs[0] != NULL)
			((PurpleRequestFileCb)data->cbs[0])(data->user_data, NULL);
		purple_request_close(data->type, data);
		return;
	}

	data->u.file.name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(data->dialog));
	current_folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(data->dialog));
	if (current_folder != NULL) {
		if (data->u.file.savedialog) {
			purple_prefs_set_path(PIDGIN_PREFS_ROOT "/filelocations/last_save_folder", current_folder);
		} else {
			purple_prefs_set_path(PIDGIN_PREFS_ROOT "/filelocations/last_open_folder", current_folder);
		}
		g_free(current_folder);
	}
	if ((data->u.file.savedialog == TRUE) &&
		(g_file_test(data->u.file.name, G_FILE_TEST_EXISTS))) {
		purple_request_action(data, NULL, _("That file already exists"),
							_("Would you like to overwrite it?"), 0,
							NULL, NULL, NULL,
							data, 2,
							_("Overwrite"), G_CALLBACK(file_yes_no_cb),
							_("Choose New Name"), G_CALLBACK(file_yes_no_cb));
	} else
		file_yes_no_cb(data, 1);
}

static void *
pidgin_request_file(const char *title, const char *filename,
					  gboolean savedialog,
					  GCallback ok_cb, GCallback cancel_cb,
					  PurpleAccount *account, const char *who, PurpleConversation *conv,
					  void *user_data)
{
	PidginRequestData *data;
	GtkWidget *filesel;
	const gchar *current_folder;
	gboolean folder_set = FALSE;

	data = g_new0(PidginRequestData, 1);
	data->type = PURPLE_REQUEST_FILE;
	data->user_data = user_data;
	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;
	data->u.file.savedialog = savedialog;

	filesel = gtk_file_chooser_dialog_new(
						title ? title : (savedialog ? _("Save File...")
													: _("Open File...")),
						NULL,
						savedialog ? GTK_FILE_CHOOSER_ACTION_SAVE
								   : GTK_FILE_CHOOSER_ACTION_OPEN,
						GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						savedialog ? GTK_STOCK_SAVE
								   : GTK_STOCK_OPEN,
						GTK_RESPONSE_ACCEPT,
						NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(filesel), GTK_RESPONSE_ACCEPT);

	if (savedialog) {
		current_folder = purple_prefs_get_path(PIDGIN_PREFS_ROOT "/filelocations/last_save_folder");
	} else {
		current_folder = purple_prefs_get_path(PIDGIN_PREFS_ROOT "/filelocations/last_open_folder");
	}

	if ((filename != NULL) && (*filename != '\0')) {
		if (savedialog)
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(filesel), filename);
		else if (g_file_test(filename, G_FILE_TEST_EXISTS))
			gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(filesel), filename);
	}
	if ((filename == NULL || *filename == '\0' || !g_file_test(filename, G_FILE_TEST_EXISTS)) &&
				(current_folder != NULL) && (*current_folder != '\0')) {
		folder_set = gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filesel), current_folder);
	}

#ifdef _WIN32
	if (!folder_set && (filename == NULL || *filename == '\0' || !g_file_test(filename, G_FILE_TEST_EXISTS))) {
		char *my_documents = wpurple_get_special_folder(CSIDL_PERSONAL);

		if (my_documents != NULL) {
			gtk_file_chooser_set_current_folder(
					GTK_FILE_CHOOSER(filesel), my_documents);

			g_free(my_documents);
		}
	}
#else
	(void)folder_set;
#endif

	g_signal_connect(G_OBJECT(GTK_FILE_CHOOSER(filesel)), "response",
					 G_CALLBACK(file_ok_check_if_exists_cb), data);

	pidgin_auto_parent_window(filesel);

	data->dialog = filesel;
	gtk_widget_show(filesel);

	return (void *)data;
}

static void *
pidgin_request_folder(const char *title, const char *dirname,
					  GCallback ok_cb, GCallback cancel_cb,
					  PurpleAccount *account, const char *who, PurpleConversation *conv,
					  void *user_data)
{
	PidginRequestData *data;
	GtkWidget *dirsel;

	data = g_new0(PidginRequestData, 1);
	data->type = PURPLE_REQUEST_FOLDER;
	data->user_data = user_data;
	data->cb_count = 2;
	data->cbs = g_new0(GCallback, 2);
	data->cbs[0] = cancel_cb;
	data->cbs[1] = ok_cb;
	data->u.file.savedialog = FALSE;

	dirsel = gtk_file_chooser_dialog_new(
						title ? title : _("Select Folder..."),
						NULL,
						GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
						GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
						GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
						NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(dirsel), GTK_RESPONSE_ACCEPT);

	if ((dirname != NULL) && (*dirname != '\0'))
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dirsel), dirname);

	g_signal_connect(G_OBJECT(GTK_FILE_CHOOSER(dirsel)), "response",
						G_CALLBACK(file_ok_check_if_exists_cb), data);

	data->dialog = dirsel;
	pidgin_auto_parent_window(dirsel);

	gtk_widget_show(dirsel);

	return (void *)data;
}

#ifdef USE_VV

#ifdef HAVE_GIOUNIX

static gboolean portal_failed;

static void screenshare_cancel_cb(GtkWidget *button, PidginRequestData *data);

static void portal_fallback(PidginRequestData *data)
{
	purple_debug_info("pidgin", "Fallback from XDP portal screenshare\n");
	portal_failed = TRUE;

	if (data->dialog) {
		pidgin_auto_parent_window(data->dialog);
		gtk_widget_show_all(data->dialog);
	} else {
		screenshare_cancel_cb(NULL, data);
	}
}

static void request_completed_cb(GObject *object, GAsyncResult *res, gpointer _data)
{
	PidginRequestData *data = _data;
	GError *error = NULL;
	GDBusMessage *msg = g_dbus_connection_send_message_with_reply_finish(data->u.screenshare.dbus_connection,
									     res,
									     &error);
	if (!msg || g_dbus_message_to_gerror(msg, &error)) {
		/* This is the expected failure mode when XDP screencast isn't available.
		 * Don't be too noisy about it; just fall back to direct mode. */
		purple_debug_info("pidgin",
				  "ScreenCast call failed: %s\n", error->message);
		portal_fallback(data);
	}
}

static gchar *portal_request_path(PidginRequestData *data, GVariantBuilder *b)
{
	const gchar *bus_name;
	gchar *dot, *request_str;
	gchar *request_path;

	bus_name = g_dbus_connection_get_unique_name(data->u.screenshare.dbus_connection);

	request_str = g_strdup_printf("u%u", data->u.screenshare.portal_request_nr++);
	if (!request_str) {
		return NULL;
	}

	request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
				       bus_name + 1, request_str);
	if (!request_path) {
		g_free(request_str);
		return NULL;
	}

	g_variant_builder_add(b, "{sv}", "handle_token", g_variant_new_take_string(request_str));

	dot = request_path;
	while ((dot = strchr(dot, '.'))) {
		*dot = '_';
	}

	return request_path;
}

static void screen_cast_call(PidginRequestData *data, const gchar *method, const gchar *str_arg,
			     GVariantBuilder *opts, GDBusSignalCallback cb)
{
	GDBusMessage *msg;
	GVariantBuilder b;
	gchar *request_path;

	if (data->u.screenshare.signal_id) {
		g_dbus_connection_signal_unsubscribe(data->u.screenshare.dbus_connection,
						     data->u.screenshare.signal_id);
	}

	if (!opts) {
		opts = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
	}

	request_path = portal_request_path(data, opts);
	if (!request_path) {
		g_variant_builder_unref(opts);
		purple_notify_error(NULL, _("Screen share error"),
				    _("Error creating screencast request"), NULL);
		screenshare_cancel_cb(NULL, data);
	}

	data->u.screenshare.signal_id = g_dbus_connection_signal_subscribe(data->u.screenshare.dbus_connection,
									   "org.freedesktop.portal.Desktop",
									   "org.freedesktop.portal.Request",
									   "Response", request_path, NULL, 0,
									   cb, data, NULL);
	g_free(request_path);

	msg = g_dbus_message_new_method_call("org.freedesktop.portal.Desktop",
					     "/org/freedesktop/portal/desktop",
					     "org.freedesktop.portal.ScreenCast",
					     method);

	g_variant_builder_init(&b, G_VARIANT_TYPE_TUPLE);
	if (data->u.screenshare.session_path) {
		g_variant_builder_add(&b, "o", data->u.screenshare.session_path);
	}
	if (str_arg) {
		g_variant_builder_add(&b, "s", str_arg);
	}
	g_variant_builder_add(&b, "a{sv}", opts);

	g_dbus_message_set_body(msg, g_variant_builder_end(&b));

	g_dbus_connection_send_message_with_reply(data->u.screenshare.dbus_connection, msg,
						  0, 2000, NULL, NULL, request_completed_cb, data);
}

static GstElement *create_pipewiresrc_cb(PurpleMedia *media, const gchar *session_id,
					 const gchar *participant)
{
	GstElement *ret;
	GObject *info;
	gchar *node_id;

	info = g_object_get_data(G_OBJECT(media), "src-element");
	if (!info) {
		return NULL;
	}

	ret = gst_element_factory_make("pipewiresrc", NULL);
	if (ret == NULL) {
		return NULL;
	}

	/* Take the node-id and fd from the PurpleMediaElementInfo
	 * and apply them to the pipewiresrc */
	node_id = g_strdup_printf("%u",
				  GPOINTER_TO_UINT(g_object_get_data(info, "node-id")));
	g_object_set(ret,"path", node_id, "do-timestamp", TRUE,
		     "fd", GPOINTER_TO_INT(g_object_get_data(info, "fd")),
		     NULL);
	g_free(node_id);

	return ret;
}

static void close_pipewire_fd(gpointer _fd)
{
	int fd = GPOINTER_TO_INT(_fd);

	close(fd);
}

static void pipewire_fd_cb(GObject *object, GAsyncResult *res, gpointer _data)
{
	PidginRequestData *data = _data;
	GError *error = NULL;
	GUnixFDList *l;
	int pipewire_fd;
	GDBusMessage *msg = g_dbus_connection_send_message_with_reply_finish(data->u.screenshare.dbus_connection,
									     res,
									     &error);
	if (!msg || g_dbus_message_to_gerror(msg, &error)) {
		purple_debug_info("pidgin", "OpenPipeWireRemote request failed: %s\n", error->message);
		purple_notify_error(NULL, _("Screen share error"),
				    _("OpenPipeWireRemote request failed"), error->message);
		g_clear_error(&error);
		screenshare_cancel_cb(NULL, data);
		return;
	}
	l = g_dbus_message_get_unix_fd_list(msg);
	if (!l) {
		purple_debug_info("pidgin", "OpenPipeWireRemote request failed to yield a file descriptor\n");
		purple_notify_error(NULL, _("Screen share error"), _("OpenPipeWireRemote request failed"),
				    _("No file descriptor found"));
		screenshare_cancel_cb(NULL, data);
		return;
	}
	pipewire_fd = g_unix_fd_list_get(l, 0, NULL);

	if (data->cbs[0] != NULL) {
		GObject *info;
		info = g_object_new(PURPLE_TYPE_MEDIA_ELEMENT_INFO,
				    "id", "screenshare-window",
				    "name", "Screen share single window",
				    "type", PURPLE_MEDIA_ELEMENT_VIDEO | PURPLE_MEDIA_ELEMENT_SRC |
				    PURPLE_MEDIA_ELEMENT_ONE_SRC,
				    "create-cb", create_pipewiresrc_cb, NULL);
		g_object_set_data_full(info, "fd", GINT_TO_POINTER(pipewire_fd), close_pipewire_fd);
		g_object_set_data(info, "node-id", GUINT_TO_POINTER(data->u.screenshare.node_id));
		/* When the DBus connection closes, the session ends. So keep it attached
		   to the PurpleMediaElementInfo, which in turn should be attached to
		   the owning PurpleMedia for the lifetime of the session. */
		g_object_set_data_full(info, "dbus-connection",
				       g_object_ref(data->u.screenshare.dbus_connection),
				       g_object_unref);
		((PurpleRequestScreenshareCb)data->cbs[0])(data->user_data, info);
	}

	purple_request_close(PURPLE_REQUEST_SCREENSHARE, data);
}


static void get_pipewire_fd(PidginRequestData *data)
{
	GDBusMessage *msg;
	GVariant *args;

	if (data->u.screenshare.signal_id) {
		g_dbus_connection_signal_unsubscribe(data->u.screenshare.dbus_connection,
						     data->u.screenshare.signal_id);
	}
	data->u.screenshare.signal_id = 0;

	msg = g_dbus_message_new_method_call("org.freedesktop.portal.Desktop",
					     "/org/freedesktop/portal/desktop",
					     "org.freedesktop.portal.ScreenCast",
					     "OpenPipeWireRemote");

	args = g_variant_new("(oa{sv})", data->u.screenshare.session_path, NULL);
	g_dbus_message_set_body(msg, args);

	g_dbus_connection_send_message_with_reply(data->u.screenshare.dbus_connection, msg,
						  0, 200, NULL, NULL, pipewire_fd_cb, data);
}

static void started_cb(GDBusConnection *dc, const gchar *sender_name,
		       const gchar *object_path, const gchar *interface_name,
		       const gchar *signal_name, GVariant *params, gpointer _data)
{
	PidginRequestData *data = _data;
	GVariant *args, *streams;
	guint code;

	g_variant_get(params, "(u@a{sv})", &code, &args);
	if (code || !g_variant_lookup(args, "streams", "@a(ua{sv})", &streams) ||
	    g_variant_n_children(streams) != 1) {
		purple_debug_info("pidgin", "Screencast Start call returned %d\n", code);
		purple_notify_error(NULL, _("Screen share error"),
				    _("Screencast \"Start\" failed"), NULL);
		screenshare_cancel_cb(NULL, data);
		return;
	}

	g_variant_get_child(streams, 0, "(u@a{sv})", &data->u.screenshare.node_id, NULL);

	get_pipewire_fd(data);
}

static void source_selected_cb(GDBusConnection *dc, const gchar *sender_name,
			       const gchar *object_path, const gchar *interface_name,
			       const gchar *signal_name, GVariant *params, gpointer _data)
{
	PidginRequestData *data = _data;
	guint code;

	g_variant_get(params, "(u@a{sv})", &code, NULL);
	if (code) {
		purple_debug_info("pidgin", "Screencast SelectSources call returned %d\n", code);
		purple_notify_error(NULL, _("Screen share error"),
				    _("Screencast \"SelectSources\" failed"), NULL);
		screenshare_cancel_cb(NULL, data);
		return;
	}

	screen_cast_call(data, "Start", "", NULL, started_cb);
}

static void sess_created_cb(GDBusConnection *dc, const gchar *sender_name,
			    const gchar *object_path, const gchar *interface_name,
			    const gchar *signal_name, GVariant *params, gpointer _data)
{
	PidginRequestData *data = _data;
	GVariantBuilder opts;
	GVariant *args;
	guint code;

	g_variant_get(params, "(u@a{sv})", &code, &args);
	if (code || !g_variant_lookup(args, "session_handle", "s",
				      &data->u.screenshare.session_path)) {
		purple_debug_info("pidgin", "Screencast CreateSession call returned %d\n", code);
		purple_notify_error(NULL, _("Screen share error"),
				    _("Screencast \"CreateSession\" failed."), NULL);
		screenshare_cancel_cb(NULL, data);
		return;
	}

	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(3));

	screen_cast_call(data, "SelectSources", NULL, &opts, source_selected_cb);
}

static void portal_conn_cb(GObject *object, GAsyncResult *res, gpointer _data)
{
	PidginRequestData *data = _data;
	GVariantBuilder opts;
	GError *error = NULL;
	gchar *session_token;

	data->u.screenshare.dbus_connection = g_dbus_connection_new_for_address_finish(res, &error);
	if (!data->u.screenshare.dbus_connection) {
		purple_debug_info("pidgin", "Connection to XDP portal failed: %s\n", error->message);
		portal_fallback(data);
		return;
	}

	session_token = g_strdup_printf("u%u", data->u.screenshare.portal_session_nr++);

	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&opts, "{sv}", "session_handle_token",
			      g_variant_new_take_string(session_token));

	screen_cast_call(data, "CreateSession", NULL, &opts, sess_created_cb);
}

static gboolean request_xdp_portal_screenshare(PidginRequestData *data)
{
	gchar *addr;

	if (portal_failed) {
		return FALSE;
	}

	data->u.screenshare.cancellable = g_cancellable_new();

	/* We create a new connection instead of using g_bus_get() because it
	 * makes cleanup a *lot* easier. Just kill the connection. */
	addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (!addr) {
		portal_failed = TRUE;
		return FALSE;
	}

	g_dbus_connection_new_for_address(addr,
					  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION |
					  G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL,
					  data->u.screenshare.cancellable, portal_conn_cb, data);
	g_free(addr);
	return TRUE;
}

#endif

static GstElement *create_screensrc_cb(PurpleMedia *media, const gchar *session_id,
				       const gchar *participant);

#ifdef HAVE_X11
static gboolean
grab_event (GtkWidget *child, GdkEvent *event, PidginRequestData *data)
{
	GdkScreen *screen = gdk_screen_get_default();
	GObject *info;
	GdkWindow *gdkroot = gdk_get_default_root_window();
	Window xroot = GDK_WINDOW_XID(gdkroot), xwindow, parent, *children;
	unsigned int nchildren, xmask;
	Display *xdisplay = GDK_SCREEN_XDISPLAY(screen);
	int rootx, rooty, winx, winy;

	if (event->type != GDK_BUTTON_PRESS)
		return FALSE;

	XQueryPointer(xdisplay, xroot, &xroot, &xwindow, &rootx, &rooty, &winx, &winy, &xmask);

	gdk_pointer_ungrab(GDK_CURRENT_TIME);

	/* Find WM window (direct child of root) */
	while (1) {
		if (!XQueryTree(xdisplay, xwindow, &xroot, &parent, &children, &nchildren))
			break;

		if (nchildren)
			XFree(children);

		if (xroot == parent)
			break;

		xwindow = parent;
	}

	generic_response_start(data);

	if (data->cbs[0] != NULL) {
		info = g_object_new(PURPLE_TYPE_MEDIA_ELEMENT_INFO,
				    "id", "screenshare-window",
				    "name", "Screen share single window",
				    "type", PURPLE_MEDIA_ELEMENT_VIDEO | PURPLE_MEDIA_ELEMENT_SRC |
				    PURPLE_MEDIA_ELEMENT_ONE_SRC,
				    "create-cb", create_screensrc_cb, NULL);
		g_object_set_data(info, "window-id", GUINT_TO_POINTER(xwindow));
		((PurpleRequestScreenshareCb)data->cbs[0])(data->user_data, info);
	}

	purple_request_close(PURPLE_REQUEST_SCREENSHARE, data);

	return FALSE;
}

static void
screenshare_window_cb(GtkWidget *button, PidginRequestData *data)
{
	GdkCursor *cursor;
	GdkWindow *gdkwin = gtk_widget_get_window(GTK_WIDGET(data->dialog));

	if (!GTK_WIDGET_HAS_FOCUS(button))
		gtk_widget_grab_focus(button);

	gtk_widget_add_events(GTK_WIDGET(data->dialog),
			      GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	g_signal_connect(data->dialog, "event", G_CALLBACK(grab_event), data);

	cursor = gdk_cursor_new(GDK_CROSSHAIR);
	gdk_pointer_grab(gdkwin, FALSE,
			 GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK,
			 NULL, cursor, GDK_CURRENT_TIME);
}

static GstElement *create_screensrc_cb(PurpleMedia *media, const gchar *session_id,
				       const gchar *participant)
{
	GObject *info;
	GstElement *ret;

	ret = gst_element_factory_make("ximagesrc", NULL);
	if (ret == NULL)
		return NULL;

	g_object_set(ret, "use-damage", 0, NULL);

	info = g_object_get_data(G_OBJECT(media), "src-element");
	if (info) {
		Window xid = GPOINTER_TO_UINT(g_object_get_data(info, "window-id"));
		int monitor_no = GPOINTER_TO_INT(g_object_get_data(info, "monitor-no"));
		if (xid) {
			g_object_set(ret, "xid", xid, NULL);
		} else if (monitor_no >= 0) {
			GdkScreen *screen = gdk_screen_get_default();
			GdkRectangle geom;

			gdk_screen_get_monitor_geometry(screen, monitor_no, &geom);
			g_object_set(ret, "startx", geom.x, "starty", geom.y,
				     "endx", geom.x + geom.width - 1,
				     "endy", geom.y + geom.height - 1, NULL);
		}
	}

	return ret;
}
#elif defined (_WIN32)
static GstElement *create_screensrc_cb(PPurpleMedia *media, const gchar *session_id,
				       const gchar *participant)
{
	GObject *info;
	GstElement *ret;

	ret = gst_element_factory_make("gdiscreencapsrc", NULL);
	if (ret == NULL)
		return NULL;

	g_object_set(ret, "cursor", TRUE);

	info = g_object_get_data(G_OBJECT(media), "src-element");
	if (info) {
		int monitor_no = GPOINTER_TO_INT(g_object_get_data(info, "monitor-no"));
		if (monitor_no >= 0)
			g_object_set(ret, "monitor", monitor_no);
	}

	return ret;
}
#else
/* We don't actually need to break the build just because we can't do
 * screencap, but gtkmedia.c is going to break the USE_VV build if it
 * isn't WIN32 or X11 anyway, so we might as well. */
#error "Unsupported windowing system"
#endif

static void
screenshare_monitor_cb(GtkWidget *button, PidginRequestData *data)
{
	GtkWidget *radio;
	GObject *info;
	int monitor_no = -1;

	generic_response_start(data);

	if (!GTK_WIDGET_HAS_FOCUS(button))
		gtk_widget_grab_focus(button);

	radio = g_object_get_data(G_OBJECT(data->dialog), "radio");
	if (radio) {
		GSList *group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(radio));

		while (group) {
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(group->data))) {
				monitor_no = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(group->data),
									       "monitor-no"));
				break;
			}
			group = group->next;
		}
	}
	if (data->cbs[0] != NULL) {
		info = g_object_new(PURPLE_TYPE_MEDIA_ELEMENT_INFO,
				    "id", "screenshare-monitor",
				    "name", "Screen share monitor",
				    "type", PURPLE_MEDIA_ELEMENT_VIDEO | PURPLE_MEDIA_ELEMENT_SRC |
				    PURPLE_MEDIA_ELEMENT_ONE_SRC,
				    "create-cb", create_screensrc_cb, NULL);
		g_object_set_data(info, "monitor-no", GINT_TO_POINTER(monitor_no));
		((PurpleRequestScreenshareCb)data->cbs[0])(data->user_data, info);
	}

	purple_request_close(PURPLE_REQUEST_SCREENSHARE, data);
}

static GstElement *create_videotest_cb(PurpleMedia *media, const gchar *session_id,
				       const gchar *participant)
{
	return gst_element_factory_make("videotestsrc", NULL);
}

static void
screenshare_videotest_cb(GtkWidget *button, PidginRequestData *data)
{
	GObject *info;

	generic_response_start(data);

	if (!GTK_WIDGET_HAS_FOCUS(button))
		gtk_widget_grab_focus(button);

	if (data->cbs[0] != NULL) {
		info = g_object_new(PURPLE_TYPE_MEDIA_ELEMENT_INFO,
				    "id", "screenshare-videotestsrc",
				    "name", "Screen share test source",
				    "type", PURPLE_MEDIA_ELEMENT_VIDEO | PURPLE_MEDIA_ELEMENT_SRC |
				    PURPLE_MEDIA_ELEMENT_ONE_SRC,
				    "create-cb", create_videotest_cb, NULL);
		((PurpleRequestScreenshareCb)data->cbs[0])(data->user_data, info);
	}

	purple_request_close(PURPLE_REQUEST_SCREENSHARE, data);
}

static void
screenshare_cancel_cb(GtkWidget *button, PidginRequestData *data)
{
	if (data->dialog) {
		generic_response_start(data);
	}

	if (data->cbs[0] != NULL)
		((PurpleRequestScreenshareCb)data->cbs[0])(data->user_data, NULL);

	purple_request_close(PURPLE_REQUEST_SCREENSHARE, data);
}

static gboolean
destroy_screenshare_cb(GtkWidget *dialog, GdkEvent *event,
		       PidginRequestData *data)
{
	screenshare_cancel_cb(NULL, data);
	return FALSE;
}

static void *pidgin_request_screenshare_media(const char *title, const char *primary,
					      const char *secondary, PurpleAccount *account,
					      GCallback cb, void *user_data)
{
	PidginRequestData *data;
	GtkWidget *dialog;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *button;
	GtkWidget *radio = NULL;
	GdkScreen *screen;
	char *label_text;
	char *primary_esc, *secondary_esc;

	data            = g_new0(PidginRequestData, 1);
	data->type      = PURPLE_REQUEST_SCREENSHARE;
	data->user_data = user_data;

	data->cb_count = 1;
	data->cbs = g_new0(GCallback, 1);
	data->cbs[0] = cb;

	/* Create the dialog. */
	data->dialog = dialog = gtk_dialog_new();

	if (title != NULL)
		gtk_window_set_title(GTK_WINDOW(dialog), title);
#ifdef _WIN32
	else
		gtk_window_set_title(GTK_WINDOW(dialog), PIDGIN_ALERT_TITLE);
#endif

	button = pidgin_dialog_add_button(GTK_DIALOG(dialog), GTK_STOCK_CANCEL,
					  G_CALLBACK(screenshare_cancel_cb), data);
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);

	if (g_getenv("PIDGIN_SHARE_VIDEOTEST") != NULL) {
		button = pidgin_dialog_add_button(GTK_DIALOG(dialog), _("Test image"),
						  G_CALLBACK(screenshare_videotest_cb), data);
		GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
		gtk_window_set_default(GTK_WINDOW(dialog), button);
	}

#ifdef HAVE_X11
	button = pidgin_dialog_add_button(GTK_DIALOG(dialog), _("Select window"),
					  G_CALLBACK(screenshare_window_cb), data);
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_window_set_default(GTK_WINDOW(dialog), button);
#endif

	button = pidgin_dialog_add_button(GTK_DIALOG(dialog), _("Use monitor"),
					  G_CALLBACK(screenshare_monitor_cb), data);
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_window_set_default(GTK_WINDOW(dialog), button);

	g_signal_connect(G_OBJECT(dialog), "delete_event",
					 G_CALLBACK(destroy_screenshare_cb), data);

	/* Setup the dialog */
	gtk_container_set_border_width(GTK_CONTAINER(dialog), PIDGIN_HIG_BORDER/2);
	gtk_container_set_border_width(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER/2);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);
	gtk_box_set_spacing(GTK_BOX(GTK_DIALOG(dialog)->vbox), PIDGIN_HIG_BORDER);

	/* Setup the main horizontal box */
	hbox = gtk_hbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), hbox);


	/* Vertical box */
	vbox = gtk_vbox_new(FALSE, PIDGIN_HIG_BORDER);
	gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);

	pidgin_widget_decorate_account(hbox, account);

	/* Descriptive label */
	primary_esc = (primary != NULL) ? g_markup_escape_text(primary, -1) : NULL;
	secondary_esc = (secondary != NULL) ? g_markup_escape_text(secondary, -1) : NULL;
	label_text = g_strdup_printf((primary ? "<span weight=\"bold\" size=\"larger\">"
								 "%s</span>%s%s" : "%s%s%s"),
								 (primary ? primary_esc : ""),
								 ((primary && secondary) ? "\n\n" : ""),
								 (secondary ? secondary_esc : ""));
	g_free(primary_esc);
	g_free(secondary_esc);

	label = gtk_label_new(NULL);

	gtk_label_set_markup(GTK_LABEL(label), label_text);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

	g_free(label_text);

	screen = gdk_screen_get_default();
	if (screen) {
		int nr_monitors = gdk_screen_get_n_monitors(screen);
		int primary = gdk_screen_get_primary_monitor(screen);
		int i;

		for (i = 0; i < nr_monitors; i++) {
			GdkRectangle geom;
			gchar *name;
			gchar *label;

			name = gdk_screen_get_monitor_plug_name(screen, i);
			gdk_screen_get_monitor_geometry(screen, i, &geom);

			label = g_strdup_printf(_("%s (%d???%d @ %d,%d)"),
						name ? name : _("Unknown output"),
						geom.width, geom.height,
						geom.x, geom.y);
			radio = gtk_radio_button_new_with_label_from_widget((GtkRadioButton *)radio, label);
			g_object_set_data(G_OBJECT(radio), "monitor-no", GINT_TO_POINTER(i));
			gtk_box_pack_start(GTK_BOX(vbox), radio, FALSE, FALSE, 0);
			if (i == primary)
			       gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);

			g_free(label);
			g_free(name);
		}
		g_object_set_data(G_OBJECT(dialog), "radio", radio);
	}

#ifdef HAVE_GIOUNIX
	/*
	 * We create the dialog for direct x11/win share here anyway, because
	 * it's simpler than storing everything we need to create it, including
	 * the PurpleAccount which we can't just take a ref on because it isn't
	 * just a GObject yet. On fallback, the dialog can be used immediately.
	 */
	if (request_xdp_portal_screenshare(data)) {
		purple_debug_info("pidgin", "Attempt XDP portal screenshare\n");
		return data;
	}
#endif

	purple_debug_info("pidgin", "Using direct screenshare\n");

	/* Show everything. */
	pidgin_auto_parent_window(dialog);

	gtk_widget_show_all(dialog);

	return data;

}
#endif /* USE_VV */

static void
pidgin_close_request(PurpleRequestType type, void *ui_handle)
{
	PidginRequestData *data = (PidginRequestData *)ui_handle;

	g_free(data->cbs);

	if (data->dialog) {
		gtk_widget_destroy(data->dialog);
	}

	if (type == PURPLE_REQUEST_FIELDS) {
		purple_request_fields_destroy(data->u.multifield.fields);
	} else if (type == PURPLE_REQUEST_FILE) {
		g_free(data->u.file.name);
	} else if (type == PURPLE_REQUEST_SCREENSHARE) {
#ifdef HAVE_GIOUNIX
		g_cancellable_cancel(data->u.screenshare.cancellable);
		if (data->u.screenshare.signal_id)
			g_dbus_connection_signal_unsubscribe(data->u.screenshare.dbus_connection,
							     data->u.screenshare.signal_id);
		g_clear_object(&data->u.screenshare.dbus_connection);
		g_free(data->u.screenshare.session_path);
		g_clear_object(&data->u.screenshare.cancellable);
#endif
	}

	g_free(data);
}

static PurpleRequestUiOps ops =
{
	pidgin_request_input,
	pidgin_request_choice,
	pidgin_request_action,
	pidgin_request_fields,
	pidgin_request_file,
	pidgin_close_request,
	pidgin_request_folder,
	pidgin_request_action_with_icon,
#ifdef USE_VV
	pidgin_request_screenshare_media,
#else
	NULL,
#endif
	NULL,
	NULL
};

PurpleRequestUiOps *
pidgin_request_get_ui_ops(void)
{
	return &ops;
}
