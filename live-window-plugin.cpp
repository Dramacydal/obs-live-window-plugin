#include <stdlib.h>
#include <util/dstr.h>
#include <obs-frontend-api.h>
#include <obs-scene.h>
#include <Dwmapi.h>
#include "dc-capture.h"
#include "window-helpers.h"

/* clang-format off */

#define TEXT_WINDOW_CAPTURE obs_module_text("LiveWindowCapture")
#define TEXT_WINDOW         obs_module_text("LiveWindowCapture.Window")
#define TEXT_MATCH_PRIORITY obs_module_text("LiveWindowCapture.Priority")
#define TEXT_MATCH_TITLE    obs_module_text("LiveWindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS    obs_module_text("LiveWindowCapture.Priority.Class")
#define TEXT_MATCH_EXE      obs_module_text("LiveWindowCapture.Priority.Exe")
#define TEXT_CAPTURE_BORDER obs_module_text("LiveCaptureBorder")
#define TEXT_CAPTURE_CURSOR obs_module_text("LiveCaptureCursor")
#define TEXT_COMPATIBILITY  obs_module_text("LiveCompatibility")

/* clang-format on */

struct live_window_capture {
	obs_source_t *source;

	char *title;
	char *class;
	char *executable;
	enum window_priority priority;
	bool border;
	bool cursor;
	bool compatibility;
	bool use_wildcards; /* TODO */

	struct dc_capture capture;

	float resize_timer;
	float check_window_timer;
	float cursor_check_time;

	int lastX;
	int lastY;

	HWND window;
	RECT last_rect;
};

static void update_settings(struct live_window_capture *wc, obs_data_t *s)
{
	const char *window = obs_data_get_string(s, "window");
	int priority = (int)obs_data_get_int(s, "priority");

	bfree(wc->title);
	bfree(wc->class);
	bfree(wc->executable);

	build_window_strings(window, &wc->class, &wc->title, &wc->executable);

	if (wc->title != NULL) {
		blog(LOG_INFO,
		     "[window-capture: '%s'] update settings:\n"
		     "\texecutable: %s",
		     obs_source_get_name(wc->source), wc->executable);
		blog(LOG_DEBUG, "\tclass:      %s", wc->class);
	}

	wc->priority = (enum window_priority)priority;
	wc->border = obs_data_get_bool(s, "border");
	wc->cursor = obs_data_get_bool(s, "cursor");
	wc->use_wildcards = obs_data_get_bool(s, "use_wildcards");
	wc->compatibility = obs_data_get_bool(s, "compatibility");
}

/* ------------------------------------------------------------------------- */

static const char *live_wc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_WINDOW_CAPTURE;
}

static void *live_wc_create(obs_data_t *settings, obs_source_t *source)
{
	struct live_window_capture *wc = bzalloc(sizeof(struct live_window_capture));
	wc->source = source;
	wc->lastX = -1;
	wc->lastY = -1;

	update_settings(wc, settings);
	return wc;
}

static void live_wc_destroy(void *data)
{
	struct live_window_capture *wc = data;

	if (wc) {
		obs_enter_graphics();
		dc_capture_free(&wc->capture);
		obs_leave_graphics();

		bfree(wc->title);
		bfree(wc->class);
		bfree(wc->executable);

		bfree(wc);
	}
}

static void live_wc_update(void *data, obs_data_t *settings)
{
	struct live_window_capture *wc = data;
	update_settings(wc, settings);

	/* forces a reset */
	wc->window = NULL;
}

static uint32_t live_wc_width(void *data)
{
	struct live_window_capture *wc = data;
	return wc->capture.width;
}

static uint32_t live_wc_height(void *data)
{
	struct live_window_capture *wc = data;
	return wc->capture.height;
}

static void live_wc_defaults(obs_data_t *defaults)
{
	obs_data_set_default_bool(defaults, "border", false);
	obs_data_set_default_bool(defaults, "cursor", true);
	obs_data_set_default_bool(defaults, "compatibility", false);
}

static obs_properties_t *live_wc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, "window", TEXT_WINDOW,
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	fill_window_list(p, EXCLUDE_MINIMIZED, NULL);

	p = obs_properties_add_list(ppts, "priority", TEXT_MATCH_PRIORITY,
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE, WINDOW_PRIORITY_EXE);

	obs_properties_add_bool(ppts, "border", TEXT_CAPTURE_BORDER);
	obs_properties_add_bool(ppts, "cursor", TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, "compatibility", TEXT_COMPATIBILITY);

	return ppts;
}

#define RESIZE_CHECK_TIME 0.2f
#define CURSOR_CHECK_TIME 0.2f

struct my_callback_param
{
	obs_source_t* sceneSource;

	struct live_window_capture* wc;
};

void updateSceneItemPosition(obs_sceneitem_t* item, struct my_callback_param* myParam)
{
	RECT rMyRect;
	if (!GetWindowRect(myParam->wc->window, (LPRECT)& rMyRect))
		return;

	if (myParam->wc->lastX != rMyRect.left && myParam->wc->lastY != rMyRect.top)
	{
		struct vec2 pos;
		pos.x = rMyRect.left;
		pos.y = rMyRect.top;
		obs_sceneitem_set_pos(item, &pos);

		myParam->wc->lastX = rMyRect.left;
		myParam->wc->lastY = rMyRect.top;
	}
}

bool reposition_shit_callback(obs_scene_t* scene, obs_sceneitem_t* item, void* param)
{
	struct my_callback_param* myParam = param;
	if (!myParam->wc->window)
		return false;

	if (item->source == myParam->wc->source)
	{
		updateSceneItemPosition(item, myParam);

		return false;
	}
	
	return true;
}

struct PositionData
{
	struct live_window_capture* wc;
	int counter;

	struct obs_sceneitem_t* lastItem;
	struct obs_sceneitem_t* currentItem;
	int lastIndex;
};

bool moveontop_shit_callback(obs_scene_t* scene, obs_sceneitem_t* item, void* data)
{
	struct PositionData* posData = data;
	if (strcmp(obs_source_get_display_name(item->source->info.id), TEXT_WINDOW_CAPTURE) != 0) {
		++posData->counter;
		return true;
	}

	posData->lastIndex = posData->counter;
	posData->lastItem = item;

	if (item->source == posData->wc->source)
		posData->currentItem = item;

	++posData->counter;

	return true;
}

void moveOnTop(obs_scene_t* currentScene2, struct live_window_capture* wc)
{
	struct PositionData data;
	data.lastItem = NULL;
	data.lastIndex = -1;
	data.wc = wc;
	data.counter = 0;
	data.currentItem = NULL;

	obs_scene_enum_items(currentScene2, moveontop_shit_callback, &data);

	if (data.currentItem != NULL && data.lastItem != data.currentItem)
	{
		obs_sceneitem_set_order_position(data.currentItem, data.lastIndex);

		/*char c[1024];
		memset(c, 0, sizeof(c));
		sprintf(c, "%u", data.lastIndex);

		MessageBoxA(NULL, c, "", 0);*/
	}
}

void do_reposition_shit(struct live_window_capture* wc)
{
	obs_source_t* currentScene = obs_frontend_get_current_scene();
	if (!currentScene)
		return;

	obs_scene_t* currentScene2 = obs_scene_from_source(currentScene);
	if (!currentScene2) {
		obs_source_release(currentScene);
		return;
	}

	struct my_callback_param param;
	param.sceneSource = currentScene;
	param.wc = wc;

	obs_scene_enum_items(currentScene2, reposition_shit_callback, &param);

	HWND hwnd = GetForegroundWindow();
	if (hwnd == wc->window)
		moveOnTop(currentScene2, wc);

	obs_source_release(currentScene);
}

static void live_wc_tick(void *data, float seconds)
{
	struct live_window_capture *wc = data;
	RECT rect;
	bool reset_capture = false;

	if (!obs_source_showing(wc->source))
		return;

	if (!wc->window || !IsWindow(wc->window)) {
		if (!wc->title && !wc->class)
			return;

		wc->check_window_timer += seconds;

		if (wc->check_window_timer < 1.0f) {
			if (wc->capture.valid)
				dc_capture_free(&wc->capture);
			return;
		}

		wc->check_window_timer = 0.0f;

		wc->window = find_window(EXCLUDE_MINIMIZED, wc->priority,
					 wc->class, wc->title, wc->executable);
		if (!wc->window) {
			if (wc->capture.valid)
				dc_capture_free(&wc->capture);
			return;
		}

		reset_capture = true;

	} else if (IsIconic(wc->window)) {
		if (wc->capture.valid)
			dc_capture_free(&wc->capture);
		return;
	} else
	{
		if (!wc->capture.valid)
			reset_capture = true;
		do_reposition_shit(wc);
	}

	wc->cursor_check_time += seconds;
	if (wc->cursor_check_time > CURSOR_CHECK_TIME) {
		DWORD foreground_pid, target_pid;

		// Can't just compare the window handle in case of app with child windows
		if (!GetWindowThreadProcessId(GetForegroundWindow(),
					      &foreground_pid))
			foreground_pid = 0;

		if (!GetWindowThreadProcessId(wc->window, &target_pid))
			target_pid = 0;

		if (foreground_pid && target_pid &&
		    foreground_pid != target_pid)
			wc->capture.cursor_hidden = true;
		else
			wc->capture.cursor_hidden = false;

		wc->cursor_check_time = 0.0f;
	}

	obs_enter_graphics();

	GetClientRect(wc->window, &rect);

	if (!reset_capture) {
		wc->resize_timer += seconds;

		if (wc->resize_timer >= RESIZE_CHECK_TIME) {
			if (rect.bottom != wc->last_rect.bottom ||
			    rect.right != wc->last_rect.right)
				reset_capture = true;

			wc->resize_timer = 0.0f;
		}
	}

	if (reset_capture) {
		wc->resize_timer = 0.0f;
		wc->last_rect = rect;
		dc_capture_free(&wc->capture);

		RECT realRect;

		if (wc->border)
		{
			DwmGetWindowAttribute(wc->window, DWMWA_EXTENDED_FRAME_BOUNDS, &realRect, sizeof(realRect));
			ScreenToClient(wc->window, &realRect.left);
			ScreenToClient(wc->window, &realRect.right);
		}
		else
			realRect = rect;

		dc_capture_init(&wc->capture, realRect.left, realRect.top, realRect.right, realRect.bottom,
				wc->cursor, wc->compatibility);
	}

	dc_capture_capture(&wc->capture, wc->window);
	obs_leave_graphics();
}

static void live_wc_render(void *data, gs_effect_t *effect)
{
	struct live_window_capture *wc = data;
	dc_capture_render(&wc->capture, obs_get_base_effect(OBS_EFFECT_OPAQUE));

	UNUSED_PARAMETER(effect);
}

struct obs_source_info live_window_capture_info = {
	.id = "live_window_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = live_wc_getname,
	.create = live_wc_create,
	.destroy = live_wc_destroy,
	.update = live_wc_update,
	.video_render = live_wc_render,
	.video_tick = live_wc_tick,
	.get_width = live_wc_width,
	.get_height = live_wc_height,
	.get_defaults = live_wc_defaults,
	.get_properties = live_wc_properties,
};

static_assert(sizeof(live_window_capture_info) == 280, "");
