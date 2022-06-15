// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include "labwc.h"
#include "node.h"
#include "ssd.h"
#include "workspaces.h"

static void
handle_commit(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, commit);
	assert(view->surface);

	/* Must receive commit signal before accessing surface->current* */
	struct wlr_surface_state *state = &view->surface->current;
	struct view_pending_move_resize *pending = &view->pending_move_resize;

	bool move_pending = pending->update_x || pending->update_y;
	bool size_changed = view->w != state->width || view->h != state->height;

	if (!move_pending && !size_changed) {
		return;
	}

	view->w = state->width;
	view->h = state->height;

	if (pending->update_x) {
		/* Adjust x for queued up configure events */
		view->x = pending->x + pending->width - view->w;
	}
	if (pending->update_y) {
		/* Adjust y for queued up configure events */
		view->y = pending->y + pending->height - view->h;
	}
	if (move_pending) {
		wlr_scene_node_set_position(&view->scene_tree->node,
			view->x, view->y);
	}
	if ((int)pending->width == view->w && (int)pending->height == view->h) {
		/*
		 * We reached the end of all queued size changing configure
		 * events
		 */
		pending->update_x = false;
		pending->update_y = false;
	}
	ssd_update_geometry(view);
}

static void
handle_request_move(struct wl_listener *listener, void *data)
{
	/*
	 * This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check
	 * the provied serial against a list of button press serials sent to
	 * this client, to prevent the client from requesting this whenever they
	 * want.
	 */
	struct view *view = wl_container_of(listener, view, request_move);
	interactive_begin(view, LAB_INPUT_STATE_MOVE, 0);
}

static void
handle_request_resize(struct wl_listener *listener, void *data)
{
	/*
	 * This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check
	 * the provied serial against a list of button press serials sent to
	 * this client, to prevent the client from requesting this whenever they
	 * want.
	 */
	struct wlr_xwayland_resize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_resize);
	interactive_begin(view, LAB_INPUT_STATE_RESIZE, event->edges);
}

static void
handle_map(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, map);
	view->impl->map(view);
}

static void
handle_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	view->impl->unmap(view);

	/*
	 * Some xwayland clients leave unmapped child views around, typically
	 * when a dialog window is closed. Although handle_destroy() is not
	 * called for these, we have to deal with them as such in terms of the
	 * foreign-toplevel protocol to avoid panels and the like still showing
	 * them.
	 */
	if (view->toplevel_handle) {
		wlr_foreign_toplevel_handle_v1_destroy(view->toplevel_handle);
		view->toplevel_handle = NULL;
	}
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, destroy);
	assert(view->type == LAB_XWAYLAND_VIEW);

	/* Reset XWayland specific surface for good measure */
	view->xwayland_surface = NULL;

	/* Remove XWayland specific handlers */
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->set_decorations.link);
	wl_list_remove(&view->override_redirect.link);
	wl_list_remove(&view->destroy.link);

	/* And finally destroy / free the view */
	view_destroy(view);
}

static void
handle_request_configure(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;

	int width = event->width;
	int height = event->height;
	view_adjust_size(view, &width, &height);

	wlr_xwayland_surface_configure(view->xwayland_surface,
		event->x, event->y, width, height);
}

static void
handle_request_activate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_activate);
	assert(view);
	desktop_focus_and_activate_view(&view->server->seat, view);
	desktop_move_to_front(view);
}

static void
handle_request_minimize(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_minimize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_minimize);
	assert(view);
	view_minimize(view, event->minimize);
}

static void
handle_request_maximize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_maximize);
	assert(view);
	view_toggle_maximize(view);
}

static void
handle_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	bool fullscreen = view->xwayland_surface->fullscreen;
	view_set_fullscreen(view, fullscreen, NULL);
}

static void
handle_set_title(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_title);
	assert(view);
	view_update_title(view);
}

static void
handle_set_class(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_app_id);
	assert(view);
	view_update_app_id(view);
}

static void
configure(struct view *view, struct wlr_box geo)
{
	view->pending_move_resize.update_x = geo.x != view->x;
	view->pending_move_resize.update_y = geo.y != view->y;
	view->pending_move_resize.x = geo.x;
	view->pending_move_resize.y = geo.y;
	view->pending_move_resize.width = geo.width;
	view->pending_move_resize.height = geo.height;
	wlr_xwayland_surface_configure(view->xwayland_surface, (int16_t)geo.x,
	       (int16_t)geo.y, (uint16_t)geo.width, (uint16_t)geo.height);
}

static void
move(struct view *view, double x, double y)
{
	view->x = x;
	view->y = y;
	struct wlr_xwayland_surface *s = view->xwayland_surface;
	wlr_xwayland_surface_configure(s, (int16_t)x, (int16_t)y,
		(uint16_t)s->width, (uint16_t)s->height);
}

static void
_close(struct view *view)
{
	wlr_xwayland_surface_close(view->xwayland_surface);
}

static const char *
get_string_prop(struct view *view, const char *prop)
{
	if (!strcmp(prop, "title")) {
		return view->xwayland_surface->title;
	}
	if (!strcmp(prop, "class")) {
		return view->xwayland_surface->class;
	}
	/* We give 'class' for wlr_foreign_toplevel_handle_v1_set_app_id() */
	if (!strcmp(prop, "app_id")) {
		return view->xwayland_surface->class;
	}
	return "";
}

static bool
want_deco(struct view *view)
{
	return view->xwayland_surface->decorations ==
	       WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
}

static void
handle_set_decorations(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_decorations);
	view_set_decorations(view, want_deco(view));
}

static void
handle_override_redirect(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, override_redirect);
	struct wlr_xwayland_surface *xsurface = data;
	struct server *server = view->server;
	bool mapped = xsurface->mapped;
	if (mapped) {
		handle_unmap(&view->unmap, NULL);
	}
	handle_destroy(&view->destroy, view);
	xsurface->data = NULL;
	struct xwayland_unmanaged *unmanaged =
		xwayland_unmanaged_create(server, xsurface);
	if (mapped) {
		unmanaged_handle_map(&unmanaged->map, xsurface);
	}
}

static void
top_left_edge_boundary_check(struct view *view)
{
	struct wlr_box deco = ssd_max_extents(view);
	if (deco.x < 0) {
		view->x -= deco.x;
	}
	if (deco.y < 0) {
		view->y -= deco.y;
	}
	struct wlr_box box = {
		.x = view->x, .y = view->y, .width = view->w, .height = view->h
	};
	view->impl->configure(view, box);
}

static void
map(struct view *view)
{
	if (view->mapped) {
		return;
	}
	view->mapped = true;
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	if (!view->fullscreen && view->xwayland_surface->fullscreen) {
		view_set_fullscreen(view, true, NULL);
	}
	if (!view->maximized && !view->fullscreen) {
		view->x = view->xwayland_surface->x;
		view->y = view->xwayland_surface->y;
		view->w = view->xwayland_surface->width;
		view->h = view->xwayland_surface->height;
	}

	if (view->surface != view->xwayland_surface->surface) {
		view->surface = view->xwayland_surface->surface;
		struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(
			view->scene_tree, view->surface);
		if (!tree) {
			/* TODO: might need further clean up */
			wl_resource_post_no_memory(view->surface->resource);
			return;
		}
		view->scene_node = &tree->node;
	}

	if (!view->toplevel_handle) {
		foreign_toplevel_handle_create(view);
	}

	if (!view->been_mapped) {
		view->ssd.enabled = want_deco(view);
		if (view->ssd.enabled) {
			view->margin = ssd_thickness(view);
		}

		if (!view->maximized && !view->fullscreen) {
			struct wlr_box box =
				output_usable_area_from_cursor_coords(view->server);
			view->x = box.x;
			view->y = box.y;
			view_center(view);
		}

		view_discover_output(view);

		if (view->ssd.enabled) {
			/* Create ssd after view_disover_output() had been called */
			ssd_create(view);
		}
		view->been_mapped = true;
	}

	if (view->ssd.enabled && !view->fullscreen && !view->maximized) {
		top_left_edge_boundary_check(view);
	}

	/* Add commit here, as xwayland map/unmap can change the wlr_surface */
	wl_signal_add(&view->xwayland_surface->surface->events.commit,
		      &view->commit);
	view->commit.notify = handle_commit;

	view_impl_map(view);
}

static void
unmap(struct view *view)
{
	if (!view->mapped) {
		return;
	}
	view->mapped = false;
	wl_list_remove(&view->commit.link);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	desktop_focus_topmost_mapped_view(view->server);
}

static void
maximize(struct view *view, bool maximized)
{
	wlr_xwayland_surface_set_maximized(view->xwayland_surface, maximized);
}

static void
set_activated(struct view *view, bool activated)
{
	struct wlr_xwayland_surface *surface = view->xwayland_surface;

	if (activated && surface->minimized) {
		wlr_xwayland_surface_set_minimized(surface, false);
	}

	wlr_xwayland_surface_activate(surface, activated);
	if (activated) {
		wlr_xwayland_surface_restack(surface, NULL, XCB_STACK_MODE_ABOVE);
	}
}

static void
set_fullscreen(struct view *view, bool fullscreen)
{
	wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, fullscreen);
}

static const struct view_impl xwl_view_impl = {
	.configure = configure,
	.close = _close,
	.get_string_prop = get_string_prop,
	.map = map,
	.move = move,
	.set_activated = set_activated,
	.set_fullscreen = set_fullscreen,
	.unmap = unmap,
	.maximize = maximize
};

void
xwayland_surface_new(struct wl_listener *listener, void *data)
{
	struct server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;
	wlr_xwayland_surface_ping(xsurface);

	/*
	 * We do not create 'views' for xwayland override_redirect surfaces,
	 * but add them to server.unmanaged_surfaces so that we can render them
	 */
	if (xsurface->override_redirect) {
		xwayland_unmanaged_create(server, xsurface);
		return;
	}

	struct view *view = calloc(1, sizeof(struct view));
	view->server = server;
	view->type = LAB_XWAYLAND_VIEW;
	view->impl = &xwl_view_impl;
	view->xwayland_surface = xsurface;

	view->workspace = server->workspace_current;
	view->scene_tree = wlr_scene_tree_create(view->workspace->tree);
	node_descriptor_create(&view->scene_tree->node,
		LAB_NODE_DESC_VIEW, view);
	xsurface->data = view;

	view->map.notify = handle_map;
	wl_signal_add(&xsurface->events.map, &view->map);
	view->unmap.notify = handle_unmap;
	wl_signal_add(&xsurface->events.unmap, &view->unmap);
	view->destroy.notify = handle_destroy;
	wl_signal_add(&xsurface->events.destroy, &view->destroy);
	view->request_configure.notify = handle_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &view->request_configure);
	view->request_activate.notify = handle_request_activate;
	wl_signal_add(&xsurface->events.request_activate, &view->request_activate);
	view->request_minimize.notify = handle_request_minimize;
	wl_signal_add(&xsurface->events.request_minimize, &view->request_minimize);
	view->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&xsurface->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen, &view->request_fullscreen);
	view->request_move.notify = handle_request_move;
	wl_signal_add(&xsurface->events.request_move, &view->request_move);
	view->request_resize.notify = handle_request_resize;
	wl_signal_add(&xsurface->events.request_resize, &view->request_resize);

	view->set_title.notify = handle_set_title;
	wl_signal_add(&xsurface->events.set_title, &view->set_title);

	view->set_app_id.notify = handle_set_class;
	wl_signal_add(&xsurface->events.set_class, &view->set_app_id);

	view->set_decorations.notify = handle_set_decorations;
	wl_signal_add(&xsurface->events.set_decorations,
			&view->set_decorations);

	view->override_redirect.notify = handle_override_redirect;
	wl_signal_add(&xsurface->events.set_override_redirect,
			&view->override_redirect);

	wl_list_insert(&view->server->views, &view->link);
}
