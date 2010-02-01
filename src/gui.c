/**
 * gui.c
 * GUI implementations.
 *
 * Copyright (C) 2009-2010 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2009-2010 Martin Szulecki <opensuse@sukimashita.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more profile.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 
 * USA
 */

#ifdef HAVE_CONFIG_H
 #include <config.h> /* for GETTEXT_PACKAGE */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <plist/plist.h>
#include <time.h>
#include <sys/time.h>
#include <glib.h>
#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>
#include <clutter/clutter.h>
#include <clutter-gtk/clutter-gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "sbmgr.h"
#include "utility.h"
#include "device.h"
#include "sbitem.h"
#include "gui.h"

#define STAGE_WIDTH 320
#define STAGE_HEIGHT 480
#define DOCK_HEIGHT 90
#define MAX_PAGE_ITEMS 16
#define PAGE_X_OFFSET(i) ((gfloat)(i)*(gfloat)(STAGE_WIDTH))

const char CLOCK_FONT[] = "FreeSans Bold 12px";
ClutterColor clock_text_color = { 255, 255, 255, 210 };

const char ITEM_FONT[] = "FreeSans Bold 10px";
ClutterColor item_text_color = { 255, 255, 255, 210 };
ClutterColor dock_item_text_color = { 255, 255, 255, 255 };
ClutterColor stage_color = { 0x00, 0x00, 0x00, 0xff };  /* Black */
ClutterColor battery_color = { 0xff, 0xff, 0xff, 0x9f };
ClutterColor spinner_color = { 0xff, 0xff, 0xff, 0xf0 };

GtkWidget *clutter_gtk_widget;

const ClutterActorBox dock_area = { 0.0, STAGE_HEIGHT - DOCK_HEIGHT, STAGE_WIDTH, STAGE_HEIGHT };

const ClutterActorBox sb_area = { 0.0, 16.0, STAGE_WIDTH, STAGE_HEIGHT - DOCK_HEIGHT - 16.0 };

const ClutterActorBox left_trigger = { -30.0, 16.0, -8.0, STAGE_HEIGHT - DOCK_HEIGHT - 16.0 };
const ClutterActorBox right_trigger = { STAGE_WIDTH + 8.0, 16.0, STAGE_WIDTH + 30.0, STAGE_HEIGHT - DOCK_HEIGHT - 16.0 };

ClutterActor *stage = NULL;
ClutterActor *the_dock = NULL;
ClutterActor *the_sb = NULL;
ClutterActor *type_label = NULL;
ClutterActor *clock_label = NULL;
ClutterActor *battery_level = NULL;
ClutterActor *page_indicator = NULL;
ClutterActor *page_indicator_group = NULL;
ClutterActor *fade_rectangle = NULL;
ClutterActor *spinner = NULL;
ClutterTimeline *spinner_timeline = NULL;
ClutterTimeline *clock_timeline = NULL;

GMutex *selected_mutex = NULL;
SBItem *selected_item = NULL;

GMutex *icon_loader_mutex = NULL;
static int icons_loaded = 0;
static int total_icons = 0;

gfloat start_x = 0.0;
gfloat start_y = 0.0;

gboolean move_left = TRUE;

GList *dockitems = NULL;
GList *sbpages = NULL;

guint num_dock_items = 0;

sbservices_client_t sbc = NULL;
device_info_t device_info = NULL;

static finished_cb_t finished_callback = NULL;
static device_info_cb_t device_info_callback = NULL;

int current_page = 0;
struct timeval last_page_switch;

static void gui_page_indicator_group_add(GList *page, int page_index);
static void gui_page_align_icons(guint page_num, gboolean animated);

/* helper */
static void sbpage_free(GList *sbitems, gpointer data)
{
    if (sbitems) {
        g_list_foreach(sbitems, (GFunc) (g_func_sbitem_free), NULL);
        g_list_free(sbitems);
        clutter_group_remove_all(CLUTTER_GROUP(page_indicator_group));
    }
}

static void pages_free()
{
    if (sbpages) {
        g_list_foreach(sbpages, (GFunc)(sbpage_free), NULL);
        g_list_free(sbpages);
        sbpages = NULL;
    }
    if (dockitems) {
        sbpage_free(dockitems, NULL);
        dockitems = NULL;
    }
}

static void clutter_actor_get_abs_center(ClutterActor *actor, gfloat *center_x, gfloat *center_y)
{
    *center_x = 0.0;
    *center_y = 0.0;
    if (!actor)
        return;
    clutter_actor_get_scale_center(actor, center_x, center_y);
    *center_x += clutter_actor_get_x(actor);
    *center_y += clutter_actor_get_y(actor);
}

static GList *iconlist_insert_item_at(GList *iconlist, SBItem *newitem, gfloat item_x, gfloat item_y, int pageindex, int icons_per_row)
{
    if (!newitem) {
        return iconlist;
    }
    
    debug_printf("%s: count items %d\n", __func__, g_list_length(iconlist));
    
    if (!iconlist) {
        debug_printf("%s: appending item\n", __func__);
        /* for empty lists just add the element */
        return g_list_append(iconlist, newitem);
    }
    gint i;
    gint count = g_list_length(iconlist);
    gint newpos = count;
    if (count <= 0) {
        return iconlist;
    }

    gfloat xpageoffset = PAGE_X_OFFSET(pageindex);

    gfloat spacing = 16;
    if (icons_per_row > 4) {
        spacing = 3;
    }

    gfloat xpos = spacing + xpageoffset;
    gfloat ypos = 16;
    gfloat oxpos = xpos;

    for (i = 0; i < count; i++) {
        oxpos = xpos;

        gint nrow = (ypos - 16) / 88;
        gint irow = (item_y - 16) / 88;

        oxpos += nrow*STAGE_WIDTH;
        gfloat ixpos = item_x + irow*STAGE_WIDTH;

        /* if required, add spacing */
        if (!move_left)
            oxpos += spacing;

        if (ixpos < oxpos + 60) {
            newpos = i;
            break;
        }

        if (((i + 1) % icons_per_row) == 0) {
            xpos = spacing + xpageoffset;
            if (ypos + 88.0 < sb_area.y2 - sb_area.y1) {
                ypos += 88.0;
            }
        } else {
            xpos += 60;
            xpos += spacing;
        }
    }

    debug_printf("%s: newpos:%d\n", __func__, newpos);

    /* do we have a full page? */
    if ((count >= MAX_PAGE_ITEMS) && (icons_per_row == 4)) {
        debug_printf("%s: full page detected\n", __func__);
        /* remove overlapping item from current page */
        SBItem *last_item = g_list_nth_data(iconlist, MAX_PAGE_ITEMS-1);
        iconlist = g_list_remove(iconlist, last_item);
        /* animate it to new position */
        ClutterActor *actor = clutter_actor_get_parent(last_item->texture);
        clutter_actor_animate(actor, CLUTTER_EASE_OUT_QUAD, 250, "x", 16.0 + PAGE_X_OFFSET(pageindex + 1), "y", 16.0, NULL);
        /* first, we need to get the pages that we have to manipulate */
        gint page_count = g_list_length(sbpages);
        gint last_index = pageindex;
        for (i = pageindex; i < page_count; i++) {
            GList *thepage = g_list_nth_data(sbpages, i);
            if (g_list_length(thepage) < 16) {
                last_index = i;
                break;
            }
        }
        if (g_list_length(g_list_nth_data(sbpages, last_index)) >= MAX_PAGE_ITEMS) {
            /* it's the last page that is full, so we need to add a new page */
            debug_printf("last page is full, appending page\n");
            sbpages = g_list_append(sbpages, NULL);
            last_index++;
        }
        debug_printf("will alter pages %d to %d (%d pages)\n", pageindex, last_index, (last_index - pageindex) + 1);
        /* now manipulate the lists in reverse order */
        for (i = last_index; i >= pageindex; i--) {
            GList *thepage = g_list_nth_data(sbpages, i);
            sbpages = g_list_remove(sbpages, thepage);
            GList *prevpage = NULL;
            if (i > pageindex) {
                prevpage = g_list_nth_data(sbpages, i-1);
            }
            gint thepage_count = g_list_length(thepage);
            while (thepage_count >= MAX_PAGE_ITEMS) {
                thepage = g_list_remove(thepage, g_list_nth_data(thepage, thepage_count-1));
                thepage_count = g_list_length(thepage);
            }
            if (prevpage) {
                SBItem *prev_page_item = g_list_nth_data(prevpage, MAX_PAGE_ITEMS-1);
                /* animate this item to fix drawing error */
                actor = clutter_actor_get_parent(prev_page_item->texture);
                clutter_actor_animate(actor, CLUTTER_LINEAR, 100, "x", 16 + PAGE_X_OFFSET(i + 1), "y", 16.0, NULL);
                thepage = g_list_prepend(thepage, prev_page_item);
            } else {
                thepage = g_list_prepend(thepage, last_item);
            }
            sbpages = g_list_insert(sbpages, thepage, i);
        }
    }
    return g_list_insert(iconlist, newitem, newpos >= MAX_PAGE_ITEMS ? 15:newpos);
}

/* clock */
static void clock_set_time(ClutterActor *label, time_t t)
{
    struct tm *curtime = localtime(&t);
    gchar *ctext = g_strdup_printf("%02d:%02d", curtime->tm_hour, curtime->tm_min);
    clutter_text_set_text(CLUTTER_TEXT(label), ctext);
    clutter_actor_set_position(label, (clutter_actor_get_width(stage) - clutter_actor_get_width(label)) / 2, 2);
    g_free(ctext);
}

static void clock_update_cb(ClutterTimeline *timeline, gint msecs, gpointer data)
{
    clock_set_time(clock_label, time(NULL));
}

/* gui */
static void gui_fade_init()
{
    ClutterColor fade_color = { 0x00, 0x00, 0x00, 0xff };
    fade_rectangle = clutter_rectangle_new_with_color(&fade_color);
    clutter_container_add_actor (CLUTTER_CONTAINER (stage), fade_rectangle);
    clutter_actor_set_position(fade_rectangle, 0, 0);
    clutter_actor_set_size(fade_rectangle, STAGE_WIDTH, STAGE_HEIGHT);
    clutter_actor_set_opacity(fade_rectangle, 0);
}

static void gui_fade_stop()
{
    clutter_actor_raise_top(fade_rectangle);
    clutter_actor_animate(CLUTTER_ACTOR(fade_rectangle), CLUTTER_EASE_OUT_QUAD, 500, "opacity", 0, NULL);
    clutter_actor_set_reactive(fade_rectangle, FALSE);
}

static void gui_fade_start()
{
    clutter_actor_set_reactive(fade_rectangle, TRUE);
    clutter_actor_raise_top(fade_rectangle);
    clutter_actor_animate(CLUTTER_ACTOR(fade_rectangle), CLUTTER_EASE_OUT_QUAD, 500, "opacity", 180, NULL);
}

static gboolean spinner_spin_cb(gpointer data)
{
    int i;
    for (i = 0; i < 12; i++) {
        ClutterActor *actor = clutter_group_get_nth_child(CLUTTER_GROUP(spinner), i);
        clutter_actor_set_opacity(actor, clutter_actor_get_opacity(actor)-30);
    }
    return TRUE;
}

static void gui_spinner_init()
{
    ClutterActor *spinner_element = clutter_rectangle_new_with_color(&spinner_color);
    clutter_actor_set_size(spinner_element, 2.0, 8.0);
    clutter_actor_hide(spinner_element);
    clutter_group_add(CLUTTER_GROUP(stage), spinner_element);

    spinner = clutter_group_new();
    int i;
    for (i = 0; i < 12; i++) {
        ClutterActor *actor = clutter_clone_new(spinner_element);
        clutter_group_add(CLUTTER_GROUP(spinner), actor);
        clutter_actor_set_position(actor, 15.0, 0.0);
        clutter_actor_set_opacity(actor, (guint8)(((gfloat)(i)/12.0)*255));
        clutter_actor_set_rotation(actor, CLUTTER_Z_AXIS, i*30, 1.0, 15.0, 0);
        clutter_actor_show(actor);
    }
    clutter_actor_hide(spinner);
    clutter_group_add(CLUTTER_GROUP(stage), spinner);
    clutter_actor_set_position(spinner, (STAGE_WIDTH-32.0)/2, (STAGE_HEIGHT-64.0)/2);
    spinner_timeline = clutter_timeline_new(100);
    clutter_timeline_set_loop(spinner_timeline, TRUE);
    g_signal_connect(spinner_timeline, "completed", G_CALLBACK(spinner_spin_cb), NULL);
}

static void gui_spinner_start()
{
    clutter_actor_show(spinner);
    clutter_actor_raise_top(spinner);
    clutter_timeline_start(spinner_timeline);
}

static void gui_spinner_stop()
{
    clutter_timeline_stop(spinner_timeline);
    clutter_actor_hide(spinner);
}

static void gui_dock_align_icons(gboolean animated)
{
    if (!dockitems)
        return;
    gint count = g_list_length(dockitems);
    if (count == 0) {
        return;
    }
    gfloat spacing = 16.0;
    gfloat ypos = 8.0;
    gfloat xpos = 0.0;
    gint i = 0;
    if (count > 4) {
        spacing = 3.0;
    }
    gfloat totalwidth = count * 60.0 + spacing * (count - 1);
    xpos = (STAGE_WIDTH - totalwidth) / 2.0;

    /* set positions */
    for (i = 0; i < count; i++) {
        SBItem *item = g_list_nth_data(dockitems, i);
        if (!item || !item->texture) {
            continue;
        }
        ClutterActor *icon = clutter_actor_get_parent(item->texture);
        if (!icon) {
            continue;
        }

        if (item != selected_item) {
            if (animated) {
                clutter_actor_animate(icon, CLUTTER_EASE_OUT_QUAD, 250, "x", xpos, "y", ypos, NULL);
            } else {
                clutter_actor_set_position(icon, xpos, ypos);
            }
        }

        xpos += 60;
        if (i < count - 1) {
            xpos += spacing;
        }
    }
}

static void gui_page_align_icons(guint page_num, gboolean animated)
{
    if (!sbpages)
        return;

    if (g_list_length(sbpages) == 0) {
        printf("%s: no pages? that's strange...\n", __func__);
        return;
    }

    GList *pageitems = g_list_nth_data(sbpages, page_num);
    if (!pageitems) {
        printf("%s: no items on page %d\n", __func__, page_num);
        return;
    }

    gint count = g_list_length(pageitems);

    gfloat ypos = 16.0;
    gfloat xpos = 16.0 + PAGE_X_OFFSET(page_num);
    gint i = 0;

    /* set positions */
    for (i = 0; i < count; i++) {
        SBItem *item = g_list_nth_data(pageitems, i);
        if (!item) {
            debug_printf("%s: item is null for i=%d\n", __func__, i);
            continue;
        }
        if (!item->texture) {
            debug_printf("%s(%d,%d): i=%d item->texture is null\n", __func__, page_num, animated, i);
            continue;
        }
        ClutterActor *icon = clutter_actor_get_parent(item->texture);
        if (!icon) {
            continue;
        }

        if (item != selected_item) {
            if (animated) {
                clutter_actor_animate(icon, CLUTTER_EASE_OUT_QUAD, 250, "x", xpos, "y", ypos, NULL);
            } else {
                clutter_actor_set_position(icon, xpos, ypos);
            }
        }

        if (((i + 1) % 4) == 0) {
            xpos = 16.0 + PAGE_X_OFFSET(page_num);
            if (ypos + 88.0 < sb_area.y2 - sb_area.y1) {
                ypos += 88.0;
            }
        } else {
            xpos += 76;
        }
    }
}

static void gui_page_indicator_group_align()
{
    gint count = clutter_group_get_n_children(CLUTTER_GROUP(page_indicator_group));
    gint i;
    gfloat xpos = 0.0;

    if (count <= 0)
        return;

    for (i = 0; i < count; i++) {
        ClutterActor *dot = clutter_group_get_nth_child(CLUTTER_GROUP(page_indicator_group), i);
        clutter_actor_set_position(dot, xpos, 0.0);
        clutter_actor_set_name(dot, g_strdup_printf("%d", i));
        if (i == current_page) {
            clutter_actor_set_opacity(dot, 200);
        } else {
            clutter_actor_set_opacity(dot, 100);
        }
        xpos += clutter_actor_get_width(dot);
    }

    clutter_actor_set_x(page_indicator_group, (STAGE_WIDTH - xpos) / 2.0);
}

static gboolean page_indicator_clicked_cb(ClutterActor *actor, ClutterButtonEvent *event, gpointer data);

static void gui_page_indicator_group_add(GList *page, int page_index)
{
    debug_printf("%s: adding page indicator for page %d\n", __func__, page_index);
    if (page_indicator) {
        ClutterActor *actor = clutter_clone_new(page_indicator);
        clutter_actor_unparent(actor);
        clutter_actor_set_reactive(actor, TRUE);
        g_signal_connect(actor,
                                "button-press-event",
                                G_CALLBACK(page_indicator_clicked_cb), NULL);
        clutter_container_add_actor(CLUTTER_CONTAINER(page_indicator_group), actor);
        gui_page_indicator_group_align();
    }
}

static void gui_page_indicator_group_remove(GList *page, int page_index)
{
    debug_printf("%s: removing page indicator for page %d\n", __func__, page_index);
    if (page_indicator) {
        ClutterActor *actor = clutter_group_get_nth_child(CLUTTER_GROUP(page_indicator_group), page_index);
        /* afaik, this also removes it from the container */
        clutter_actor_destroy(actor);
        gui_page_indicator_group_align();
    }
}

static void gui_pages_remove_empty()
{
    gint count = g_list_length(sbpages);
    gint i;
    GList *page = NULL;

    for (i = 0; i < count; i++) {
        page = g_list_nth_data(sbpages, i);
        debug_printf("%s: checking page %d itemcount %d\n", __func__, i, g_list_length(page));
        if (g_list_length(page) == 0) {
            debug_printf("%s: removing page %d\n", __func__, i);
            gui_page_indicator_group_remove(page, i);
        }
    }
    sbpages = g_list_remove_all(sbpages, NULL);
}

static void gui_set_current_page(int pageindex, gboolean animated)
{
    gint count = clutter_group_get_n_children(CLUTTER_GROUP(page_indicator_group));

    if ((pageindex < 0) || (pageindex >= count))
        return;

    /* make sure the page has correct aligned icons */
    gui_page_align_icons(pageindex, FALSE);

    current_page = pageindex;

    gui_page_indicator_group_align();

    if (animated) {
        clutter_actor_animate(the_sb, CLUTTER_EASE_IN_OUT_CUBIC, 400, "x", (gfloat) (-PAGE_X_OFFSET(current_page)), NULL);
    } else {
        clutter_actor_set_x(the_sb, (gfloat)(-PAGE_X_OFFSET(current_page)));
    }
}

static void gui_show_next_page()
{
    gui_set_current_page(current_page+1, TRUE);
}

static void gui_show_previous_page()
{
    gui_set_current_page(current_page-1, TRUE);
}

plist_t gui_get_iconstate()
{
    plist_t iconstate = NULL;
    plist_t pdockarray = NULL;
    plist_t pdockitems = NULL;
    guint i;

    guint count = g_list_length(dockitems);
    pdockitems = plist_new_array();
    for (i = 0; i < count; i++) {
        SBItem *item = g_list_nth_data(dockitems, i);
        if (!item) {
            continue;
        }
        plist_t valuenode = plist_dict_get_item(item->node, "displayIdentifier");
        if (!valuenode) {
            printf("could not get displayIdentifier\n");
            continue;
        }

        plist_t pitem = plist_new_dict();
        plist_dict_insert_item(pitem, "displayIdentifier", plist_copy(valuenode));
        plist_array_append_item(pdockitems, pitem);
    }
    for (i = count; i < num_dock_items; i++) {
        plist_array_append_item(pdockitems, plist_new_bool(0));
    }
    pdockarray = plist_new_array();
    plist_array_append_item(pdockarray, pdockitems);

    iconstate = plist_new_array();
    plist_array_append_item(iconstate, pdockarray);

    for (i = 0; i < g_list_length(sbpages); i++) {
        GList *page = g_list_nth_data(sbpages, i);
        if (page) {
            guint j;
            count = g_list_length(page);
            if (count <= 0) {
                continue;
            }
            plist_t ppage = plist_new_array();
            plist_t row = NULL;
            for (j = 0; j < 16; j++) {
                SBItem *item = g_list_nth_data(page, j);
                if ((j % 4) == 0) {
                    row = plist_new_array();
                    plist_array_append_item(ppage, row);
                }
                if (item && item->node) {
                    plist_t valuenode = plist_dict_get_item(item->node,
                                                            "displayIdentifier");
                    if (!valuenode) {
                        printf("could not get displayIdentifier\n");
                        continue;
                    }

                    plist_t pitem = plist_new_dict();
                    plist_dict_insert_item(pitem, "displayIdentifier", plist_copy(valuenode));
                    plist_array_append_item(row, pitem);
                } else {
                    plist_array_append_item(row, plist_new_bool(0));
                }
            }
            plist_array_append_item(iconstate, plist_copy(ppage));
            plist_free(ppage);
        }
    }

    return iconstate;
}

/* input */
static gboolean stage_motion_cb(ClutterActor *actor, ClutterMotionEvent *event, gpointer user_data)
{
    /* check if an item has been raised */
    if (!selected_item) {
        return FALSE;
    }

    ClutterActor *icon = clutter_actor_get_parent(selected_item->texture);

    clutter_actor_move_by(icon, (event->x - start_x), (event->y - start_y));

    if (event->x-start_x > 0) {
        move_left = FALSE;
    } else {
        move_left = TRUE;
    }

    start_x = event->x;
    start_y = event->y;

    gfloat center_x;
    gfloat center_y;
    clutter_actor_get_abs_center(icon, &center_x, &center_y);

    if (clutter_actor_box_contains(&left_trigger, center_x-30, center_y)) {
        if (current_page > 0) {
            if (elapsed_ms(&last_page_switch, 1000)) {
                gui_show_previous_page();
                gettimeofday(&last_page_switch, NULL);
            }
        }
    } else if (clutter_actor_box_contains(&right_trigger, center_x+30, center_y)) {
        if (current_page < (gint)(g_list_length(sbpages)-1)) {
            if (elapsed_ms(&last_page_switch, 1000)) {
                gui_show_next_page();
                gettimeofday(&last_page_switch, NULL);
            }
        }
    }

    if (selected_item->is_dock_item) {
        dockitems = g_list_remove(dockitems, selected_item);
        if (center_y >= dock_area.y1) {
            debug_printf("%s: icon from dock moving inside the dock!\n", __func__);
            selected_item->is_dock_item = TRUE;
            dockitems =
                iconlist_insert_item_at(dockitems, selected_item, (center_x - dock_area.x1), (center_y - dock_area.y1), 0, num_dock_items);
            gui_dock_align_icons(TRUE);
        } else {
            debug_printf("%s: icon from dock moving outside the dock!\n", __func__);
            selected_item->is_dock_item = FALSE;
            gui_page_align_icons(current_page, TRUE);
        }
    } else {
        int p = current_page;
        int i;
        GList *pageitems = NULL;
        debug_printf("%s: current_page %d\n", __func__, p);
        /* remove selected_item from all pages */
        int count = g_list_length(sbpages);
        for (i = 0; i < count; i++) {
            pageitems = g_list_nth_data(sbpages, i);
            sbpages = g_list_remove(sbpages, pageitems);
            pageitems = g_list_remove(pageitems, selected_item);
            sbpages = g_list_insert(sbpages, pageitems, i);
        }
        /* get current page */
        pageitems = g_list_nth_data(sbpages, p);
        /* remove current page from pages list as we will alter it */
        sbpages = g_list_remove(sbpages, pageitems);
        if (center_y >= dock_area.y1 && (g_list_length(dockitems) < num_dock_items)) {
            debug_printf("%s: regular icon is moving inside the dock!\n", __func__);
            selected_item->is_dock_item = TRUE;
        } else {
            debug_printf("%s: regular icon is moving!\n", __func__);
            pageitems =
                iconlist_insert_item_at(pageitems, selected_item, (center_x - sb_area.x1) + PAGE_X_OFFSET(p), (center_y - sb_area.y1), p, 4);
        }
        /* insert back current page */
        sbpages = g_list_insert(sbpages, pageitems, p);
        gui_dock_align_icons(TRUE);
        gui_page_align_icons(p, TRUE);
    }

    return TRUE;
}

static gboolean page_indicator_clicked_cb(ClutterActor *actor, ClutterButtonEvent *event, gpointer data)
{
    if (event->click_count > 1) {
        return FALSE;
    }
    const gchar *index_str = clutter_actor_get_name(actor);
    int pageindex = strtol(index_str, NULL, 10);
    debug_printf("page indicator for page %d has been clicked\n", pageindex);
    gui_set_current_page(pageindex, TRUE);
    return TRUE;
}

static gboolean item_button_press_cb(ClutterActor *actor, ClutterButtonEvent *event, gpointer user_data)
{
    if (!user_data) {
        return FALSE;
    }

    if (selected_item) {
        /* do not allow a button_press event without a prior release */
        return FALSE;
    }

    /* discard double clicks */
    if (event->click_count > 1) {
        return FALSE;
    }

    SBItem *item = (SBItem*)user_data;

    char *strval = sbitem_get_display_name(item);

    g_mutex_lock(selected_mutex);
    debug_printf("%s: %s mouse pressed\n", __func__, strval);

    if (actor) {
        gfloat diffx = 0.0;
        gfloat diffy = 0.0;
        ClutterActor *sc = clutter_actor_get_parent(actor);
        if (item->is_dock_item) {
            GList *children = clutter_container_get_children(CLUTTER_CONTAINER(sc));
            if (children) {
                ClutterActor *icon = g_list_nth_data(children, 0);
                ClutterActor *label = g_list_nth_data(children, 1);
                clutter_text_set_color(CLUTTER_TEXT(label), &item_text_color);
                clutter_actor_set_y(label, clutter_actor_get_y(icon) + 62.0);
                g_list_free(children);
            }
            diffx = dock_area.x1;
            diffy = dock_area.y1;
        } else {
            diffx = sb_area.x1 - PAGE_X_OFFSET(current_page);
            diffy = sb_area.y1;
        }
        clutter_actor_reparent(sc, stage);
        clutter_actor_set_position(sc, clutter_actor_get_x(sc) + diffx, clutter_actor_get_y(sc) + diffy);
        clutter_actor_raise_top(sc);
        clutter_actor_set_scale_full(sc, 1.2, 1.2,
                                     clutter_actor_get_x(actor) +
                                     clutter_actor_get_width(actor) / 2,
                                     clutter_actor_get_y(actor) + clutter_actor_get_height(actor) / 2);
        clutter_actor_set_opacity(sc, 160);
        selected_item = item;
        start_x = event->x;
        start_y = event->y;
    }
    g_mutex_unlock(selected_mutex);

    /* add pages and page indicators as needed */
    GList *page = NULL;
    gui_page_indicator_group_add(page, g_list_length(sbpages));
    sbpages = g_list_append(sbpages, page);

    return TRUE;
}

static gboolean item_button_release_cb(ClutterActor *actor, ClutterButtonEvent *event, gpointer user_data)
{
    if (!user_data) {
        return FALSE;
    }

    /* discard double clicks */
    if (event->click_count > 1) {
        return FALSE;
    }

    SBItem *item = (SBItem*)user_data;
    char *strval = sbitem_get_display_name(item);

    /* remove empty pages and page indicators as needed */
    gui_pages_remove_empty();
    int count = g_list_length(sbpages);
    if (current_page >= count) {
        gui_set_current_page(count-1, FALSE);
    }

    g_mutex_lock(selected_mutex);
    debug_printf("%s: %s mouse released\n", __func__, strval);

    if (actor) {
        ClutterActor *sc = clutter_actor_get_parent(actor);
        clutter_actor_set_scale_full(sc, 1.0, 1.0,
                                     clutter_actor_get_x(actor) +
                                     clutter_actor_get_width(actor) / 2,
                                     clutter_actor_get_y(actor) + clutter_actor_get_height(actor) / 2);
        clutter_actor_set_opacity(sc, 255);
        if (item->is_dock_item) {
            GList *children = clutter_container_get_children(CLUTTER_CONTAINER(sc));
            if (children) {
                ClutterActor *icon = g_list_nth_data(children, 0);
                ClutterActor *label = g_list_nth_data(children, 1);
                clutter_text_set_color(CLUTTER_TEXT(label), &dock_item_text_color);
                clutter_actor_set_y(label, clutter_actor_get_y(icon) + 67.0);
                g_list_free(children);
            }
            clutter_actor_reparent(sc, the_dock);
            clutter_actor_set_position(sc,
                                       clutter_actor_get_x(sc) - dock_area.x1, clutter_actor_get_y(sc) - dock_area.y1);
        } else {
            clutter_actor_reparent(sc, the_sb);
            clutter_actor_set_position(sc,
                                       clutter_actor_get_x(sc) +
                                       PAGE_X_OFFSET(current_page) - sb_area.x1, clutter_actor_get_y(sc) - sb_area.y1);
        }
    }

    selected_item = NULL;
    gui_dock_align_icons(TRUE);
    gui_page_align_icons(current_page, TRUE);
    start_x = 0.0;
    start_y = 0.0;

    g_mutex_unlock(selected_mutex);

    return TRUE;
}

static gboolean stage_key_press_cb(ClutterActor *actor, ClutterEvent *event, gpointer user_data)
{
    if (!user_data || (event->type != CLUTTER_KEY_PRESS)) {
        return FALSE;
    }

    guint symbol = clutter_event_get_key_symbol(event);
    switch(symbol) {
        case CLUTTER_Right:
        gui_show_next_page();
        break;
        case CLUTTER_Left:
        gui_show_previous_page();
        break;
        default:
        return FALSE;
    }
    return TRUE;
}

static void gui_show_icons()
{
    guint i;
    guint j;
    gfloat ypos;
    gfloat xpos;

    if (dockitems) {
        xpos = 0.0;
        ypos = 0.0;
        debug_printf("%s: showing dock icons\n", __func__);
        for (i = 0; i < g_list_length(dockitems); i++) {
            SBItem *item = (SBItem*)g_list_nth_data(dockitems, i);
            if (item && item->texture && !CLUTTER_ACTOR_IS_VISIBLE(item->texture) && item->node) {
                item->is_dock_item = TRUE;
                ClutterActor *grp = clutter_group_new();
                ClutterActor *actor = item->texture;
                clutter_container_add_actor(CLUTTER_CONTAINER(grp), actor);
                clutter_actor_set_position(actor, xpos, ypos);
                clutter_actor_set_reactive(actor, TRUE);
                g_signal_connect(actor, "button-press-event", G_CALLBACK(item_button_press_cb), item);
                g_signal_connect(actor, "button-release-event", G_CALLBACK(item_button_release_cb), item);
                clutter_actor_show(actor);
                actor = item->label;
                clutter_actor_set_position(actor, xpos + (59.0 - clutter_actor_get_width(actor)) / 2, ypos + 67.0);
                clutter_text_set_color(CLUTTER_TEXT(actor), &dock_item_text_color);
                clutter_actor_show(actor);
                clutter_container_add_actor(CLUTTER_CONTAINER(grp), actor);
                clutter_container_add_actor(CLUTTER_CONTAINER(the_dock), grp);
            }
        }
        gui_dock_align_icons(FALSE);
    }
    clutter_stage_ensure_redraw(CLUTTER_STAGE(stage));
    if (sbpages) {
        debug_printf("%s: processing %d pages\n", __func__, g_list_length(sbpages));
        for (j = 0; j < g_list_length(sbpages); j++) {
            GList *cpage = g_list_nth_data(sbpages, j);
            ypos = 0.0;
            xpos = 0.0;
            debug_printf("%s: showing page icons for page %d\n", __func__, j);
            for (i = 0; i < g_list_length(cpage); i++) {
                SBItem *item = (SBItem*)g_list_nth_data(cpage, i);
                if (item && item->texture && !CLUTTER_ACTOR_IS_VISIBLE(item->texture) && item->node) {
                    item->is_dock_item = FALSE;
                    ClutterActor *grp = clutter_group_new();
                    ClutterActor *actor = item->texture;
                    clutter_container_add_actor(CLUTTER_CONTAINER(grp), actor);
                    clutter_actor_set_position(actor, xpos, ypos);
                    clutter_actor_set_reactive(actor, TRUE);
                    g_signal_connect(actor, "button-press-event", G_CALLBACK(item_button_press_cb), item);
                    g_signal_connect(actor, "button-release-event", G_CALLBACK(item_button_release_cb), item);
                    clutter_actor_show(actor);
                    actor = item->label;
                    clutter_text_set_color(CLUTTER_TEXT(actor), &item_text_color);
                    clutter_actor_set_position(actor, xpos + (59.0 - clutter_actor_get_width(actor)) / 2, ypos + 62.0);
                    clutter_actor_show(actor);
                    clutter_container_add_actor(CLUTTER_CONTAINER(grp), actor);
                    clutter_container_add_actor(CLUTTER_CONTAINER(the_sb), grp);
                }
            }
            gui_page_align_icons(j, FALSE);
        }
    }
    clutter_stage_ensure_redraw(CLUTTER_STAGE(stage));
}

static gboolean sbitem_texture_new(gpointer data)
{
    SBItem *item = (SBItem *)data;
    char *icon_filename = sbitem_get_icon_filename(item);
    GError *err = NULL;

    /* create and load texture */
    ClutterActor *actor = clutter_texture_new();
    clutter_texture_set_load_async(CLUTTER_TEXTURE(actor), TRUE);
    clutter_texture_set_from_file(CLUTTER_TEXTURE(actor), icon_filename, &err);

    /* create item */
    item->texture = actor;

    char *txtval = sbitem_get_display_name(item);
    if (txtval) {
        item->label = clutter_text_new_with_text(ITEM_FONT, txtval);
    }
    if (err) {
        fprintf(stderr, "ERROR: %s\n", err->message);
        g_error_free(err);
    }

    /* FIXME: Optimize! Do not traverse whole iconlist, just this icon */
    gui_show_icons();

    g_mutex_lock(icon_loader_mutex);
    icons_loaded++;
    g_mutex_unlock(icon_loader_mutex);

    return FALSE;
}

static gpointer sbitem_thread_load_texture(gpointer data)
{
    SBItem *item = (SBItem *)data;
    char *icon_filename = sbitem_get_icon_filename(item);
    char *display_identifier = sbitem_get_display_identifier(item);
    GError *err = NULL;

    debug_printf("%s: loading icon texture for '%s'\n", __func__, display_identifier);

    if (device_sbs_save_icon(sbc, display_identifier, icon_filename, &err)) {
        /* load texture in the clutter main loop */
        clutter_threads_add_idle((GSourceFunc)sbitem_texture_new, item);
    } else {
        fprintf(stderr, "ERROR: %s\n", err->message);
        g_error_free(err);
    }
    g_free(icon_filename);

    return NULL;
}

static guint gui_load_icon_row(plist_t items, GList **row)
{
    int i;
    int count;
    int icon_count = 0;
    SBItem *item = NULL;

    count = plist_array_get_size(items);
    for (i = 0; i < count; i++) {
        plist_t icon_info = plist_array_get_item(items, i);
        item = sbitem_new(icon_info);
        if (item != NULL) {
            /* load texture of icon in a new thread */
            g_thread_create(sbitem_thread_load_texture, item, FALSE, NULL);

            *row = g_list_append(*row, item);
            icon_count++;
        }
    }

    return icon_count;
}

static void gui_set_iconstate(plist_t iconstate)
{
    int total;

    /* get total number of pages */
    total = plist_array_get_size(iconstate);

    if (total < 1) {
        fprintf(stderr, "ERROR: No icons returned in icon state\n");
        return;
    } else {
        plist_t dock = plist_array_get_item(iconstate, 0);
        if ((plist_get_node_type(dock) != PLIST_ARRAY)
                || (plist_array_get_size(dock) < 1)) {
            fprintf(stderr, "ERROR: error getting outer dock icon array!\n");
            return;
        }
        dock = plist_array_get_item(dock, 0);
        if (plist_get_node_type(dock) != PLIST_ARRAY) {
            fprintf(stderr, "ERROR: error getting inner dock icon array!\n");
            return;
        }

        /* load dock icons */
        debug_printf("%s: processing dock\n", __func__);
        num_dock_items = gui_load_icon_row(dock, &dockitems);
        total_icons += num_dock_items;
        if (total > 1) {
            /* get all page icons */
            int p, r, rows;
            for (p = 1; p < total; p++) {
                plist_t npage = plist_array_get_item(iconstate, p);
                GList *page = NULL;
                if ((plist_get_node_type(npage) != PLIST_ARRAY)
                        || (plist_array_get_size(npage) < 1)) {
                        fprintf(stderr, "ERROR: error getting outer page icon array!\n");
                        return;
                }
                /* rows */
                rows = plist_array_get_size(npage);
                for (r = 0; r < rows; r++) {
                        debug_printf("%s: processing page %d, row %d\n", __func__, p, r);

                        plist_t nrow = plist_array_get_item(npage, r);
                        if (plist_get_node_type(nrow) != PLIST_ARRAY) {
                            fprintf(stderr, "ERROR: error getting page row icon array!\n");
                            return;
                        }
                        total_icons += gui_load_icon_row(nrow, &page);
                }

                if (page) {
                        sbpages = g_list_append(sbpages, page);
                        gui_page_indicator_group_add(page, p - 1);
                }
            }
        }
    }
}

static void gui_disable_controls()
{
    gui_fade_start();
    gui_spinner_start();
}

static void gui_enable_controls()
{
    gui_spinner_stop();
    gui_fade_stop();
}

static gboolean wait_icon_load_finished(gpointer user_data)
{
    gboolean res = TRUE;
    g_mutex_lock(icon_loader_mutex);
    debug_printf("%d of %d icons loaded (%d%%)\n", icons_loaded, total_icons, (int)(100*((double)icons_loaded/(double)total_icons)));
    if (icons_loaded >= total_icons) {
        gui_enable_controls();
        res = FALSE;
        if (finished_callback) {
            finished_callback(TRUE);
            finished_callback = NULL;
        }
    }
    g_mutex_unlock(icon_loader_mutex);
    return res;
}

static gboolean gui_pages_init_cb(gpointer user_data)
{
    const char *uuid = (const char*)user_data;
    GError *error = NULL;
    plist_t iconstate = NULL;

    gui_disable_controls();
    icons_loaded = 0;
    total_icons = 0;

    pages_free();

    /* connect to sbservices */
    if (!sbc)
        sbc = device_sbs_new(uuid, &error);

    if (error) {
        g_printerr("%s", error->message);
        g_error_free(error);
        error = NULL;
    }

    if (sbc) {
        /* Load icon data */
        if (device_sbs_get_iconstate(sbc, &iconstate, &error)) {
            gui_set_iconstate(iconstate);
            plist_free(iconstate);
        }
    }

    if (error) {
        g_printerr("%s", error->message);
        g_error_free(error);
        error = NULL;
    }

    clutter_threads_add_timeout(500, (GSourceFunc)wait_icon_load_finished, NULL);

    return FALSE;
}

static gboolean update_device_info_cb(gpointer user_data)
{
    device_info_t di = (device_info_t)user_data;
    if (di) {
        clutter_text_set_text(CLUTTER_TEXT(type_label), di->device_type);
    } else {
        clutter_text_set_text(CLUTTER_TEXT(type_label), NULL);
    }
    return FALSE;
}

static gboolean update_battery_info_cb(gpointer user_data)
{
    const char *uuid = (const char*)user_data;
    GError *error = NULL;
    gboolean res = TRUE;

    if (device_get_info(uuid, &device_info, &error)) {
        clutter_actor_set_size(battery_level, (guint) (((double) (device_info->battery_capacity) / 100.0) * 15), 6);
        if (device_info->battery_capacity == 100) {
            res = FALSE;
        }
    }
    return res;
}

static gboolean init_battery_info_cb(gpointer user_data)
{
    clutter_actor_set_size(battery_level, (guint) (((double) (device_info->battery_capacity) / 100.0) * 15), 6);
    return FALSE;
}

void gui_pages_free()
{
    clutter_threads_add_timeout(0, (GSourceFunc)(update_device_info_cb), NULL);
    pages_free();
    if (sbc) {
        device_sbs_free(sbc);
	sbc = NULL;
    }
}

static gboolean device_info_cb(gpointer user_data)
{
    GError *error = NULL;
    const char *uuid = (const char*)user_data;
    if (device_get_info(uuid, &device_info, &error)) {
        /* Update device info */
        clutter_threads_add_idle((GSourceFunc)update_device_info_cb, device_info);
        /* Update battery information */
        clutter_threads_add_idle((GSourceFunc)init_battery_info_cb, NULL);
        /* Register battery state read timeout */
        clutter_threads_add_timeout(device_info->battery_poll_interval * 1000, (GSourceFunc)update_battery_info_cb, (gpointer)uuid);

	if (device_info_callback) {
            device_info_callback(device_info->device_name, device_info->device_name);
            device_info_callback = NULL;
	}
    } else {
        if (error) {
            g_printerr("%s", error->message);
            g_error_free(error);
        } else {
            g_printerr(_("Unknown error occurred"));
        }
        if (finished_callback) {
            finished_callback(FALSE);
	    finished_callback = NULL;
        }
    }
    return FALSE;
}

void gui_pages_load(const char *uuid, device_info_cb_t info_cb, finished_cb_t finished_cb)
{
    printf("%s: %s\n", __func__, uuid);
    finished_callback = finished_cb;
    device_info_callback = info_cb;

    /* Load icons */
    clutter_threads_add_idle((GSourceFunc)gui_pages_init_cb, (gpointer)uuid);

    /* Load device information */
    g_thread_create((GThreadFunc)device_info_cb, (gpointer)uuid, FALSE, NULL);
}

GtkWidget *gui_init()
{
    device_info = device_info_new();
    ClutterActor *actor;

    if (!g_thread_supported())
        g_thread_init(NULL);

    icon_loader_mutex = g_mutex_new();

    /* initialize clutter threading environment */
    clutter_threads_init();

    if (gtk_clutter_init(NULL, NULL) != CLUTTER_INIT_SUCCESS) {
        g_error("Unable to initialize GtkClutter");
    }

    gettimeofday(&last_page_switch, NULL);

    /* Create the clutter widget */
    GtkWidget *clutter_widget = gtk_clutter_embed_new();

    /* Set the size of the widget, because we should not set the size of its
     * stage when using GtkClutterEmbed.
     */
    gtk_widget_set_size_request(clutter_widget, STAGE_WIDTH, STAGE_HEIGHT);

    /* Set the stage background color */
    stage = gtk_clutter_embed_get_stage(GTK_CLUTTER_EMBED(clutter_widget));
    clutter_stage_set_color(CLUTTER_STAGE(stage), &stage_color);

    /* attach to stage signals */
    g_signal_connect(stage, "motion-event", G_CALLBACK(stage_motion_cb), NULL);
    g_signal_connect(stage, "key-press-event", G_CALLBACK(stage_key_press_cb), NULL);

    /* Load ui background */
    GError *err = NULL;
    actor = clutter_texture_new();
    clutter_texture_set_load_async(CLUTTER_TEXTURE(actor), TRUE);
    clutter_texture_set_from_file(CLUTTER_TEXTURE(actor), SBMGR_DATA "/background.png", &err);
    if (err) {
        g_error_free(err);
        err = NULL;
    }
    if (actor) {
        clutter_actor_set_position(actor, 0, 0);
        clutter_actor_show(actor);
        clutter_group_add(CLUTTER_GROUP(stage), actor);
    } else {
        fprintf(stderr, "could not load background.png\n");
    }

    /* Create device type widget */
    type_label = clutter_text_new_full(CLOCK_FONT, "", &clock_text_color);
    clutter_group_add(CLUTTER_GROUP(stage), type_label);
    clutter_actor_set_position(type_label, 3.0, 2.0);

    /* clock widget */
    clock_label = clutter_text_new_full(CLOCK_FONT, "00:00", &clock_text_color);
    clutter_group_add(CLUTTER_GROUP(stage), clock_label);

    /* page indicator group for holding the page indicator dots */
    page_indicator_group = clutter_group_new();
    clutter_group_add(CLUTTER_GROUP(stage), page_indicator_group);

    /* alignment will be done when new indicators are added */
    clutter_actor_set_position(page_indicator_group, 0, STAGE_HEIGHT - DOCK_HEIGHT - 18);

    /* page indicator (dummy), will be cloned when the pages are created */
    page_indicator = clutter_texture_new();
    clutter_texture_set_load_async(CLUTTER_TEXTURE(page_indicator), TRUE);
    clutter_texture_set_from_file(CLUTTER_TEXTURE(page_indicator), SBMGR_DATA "/dot.png", &err);
    if (err) {
        fprintf(stderr, "Could not load texture " SBMGR_DATA "/dot.png" ": %s\n", err->message);
        g_error_free(err);
        err = NULL;
    }
    if (page_indicator) {
        clutter_actor_hide(page_indicator);
        clutter_container_add_actor(CLUTTER_CONTAINER(stage), page_indicator);
    }

    /* a group for the springboard icons */
    the_sb = clutter_group_new();
    clutter_group_add(CLUTTER_GROUP(stage), the_sb);
    clutter_actor_set_position(the_sb, 0, 16);

    /* a group for the dock icons */
    the_dock = clutter_group_new();
    clutter_group_add(CLUTTER_GROUP(stage), the_dock);
    clutter_actor_set_position(the_dock, dock_area.x1, dock_area.y1);

    gui_fade_init();
    gui_spinner_init();

    /* Show the stage */
    clutter_actor_show(stage);

    /* Create a timeline to manage animation */
    clock_timeline = clutter_timeline_new(200);
    clutter_timeline_set_loop(clock_timeline, TRUE);  /* have it loop */

    /* fire a callback for frame change */
    g_signal_connect(clock_timeline, "completed", G_CALLBACK(clock_update_cb), NULL);

    /* and start it */
    clutter_timeline_start(clock_timeline);

    selected_mutex = g_mutex_new();

    /* Position and update the clock */
    clock_set_time(clock_label, time(NULL));
    clutter_actor_show(clock_label);

    /* battery capacity */
    battery_level = clutter_rectangle_new_with_color(&battery_color);
    clutter_group_add(CLUTTER_GROUP(stage), battery_level);
    clutter_actor_set_position(battery_level, 298, 6);

    return clutter_widget;
}

void gui_deinit()
{
    clutter_timeline_stop(clock_timeline);
    device_info_free(device_info);
}
