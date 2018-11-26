
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"

#include <stdio.h>
#include <stdlib.h>

#include <cairo.h>
#include <cairo-xlib.h>

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "./gx_valvecaster.h"

#define CONTROLS VARI

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef max
#define max(x, y) ((x) < (y) ? (y) : (x))
#endif

// define some MACROS to read png data from binary stream 
// png's been linked in with
// ld -r -b binary name.png -o name.o

#ifdef __APPLE__
#include <mach-o/getsect.h>

#define EXTLD(NAME) \
  extern const unsigned char _section$__DATA__ ## NAME [];
#define LDVAR(NAME) _section$__DATA__ ## NAME
#define LDLEN(NAME) (getsectbyname("__DATA", "__" #NAME)->size)

#elif (defined __WIN32__)  /* mingw */

#define EXTLD(NAME) \
  extern const unsigned char binary_ ## NAME ## _start[]; \
  extern const unsigned char binary_ ## NAME ## _end[];
#define LDVAR(NAME) \
  binary_ ## NAME ## _start
#define LDLEN(NAME) \
  ((binary_ ## NAME ## _end) - (binary_ ## NAME ## _start))

#else /* gnu/linux ld */

#define EXTLD(NAME) \
  extern const unsigned char _binary_ ## NAME ## _start[]; \
  extern const unsigned char _binary_ ## NAME ## _end[];
#define LDVAR(NAME) \
  _binary_ ## NAME ## _start
#define LDLEN(NAME) \
  ((_binary_ ## NAME ## _end) - (_binary_ ## NAME ## _start))
#endif

// png's linked in as binarys
EXTLD(pedal_png)
EXTLD(pswitch_on_png)
EXTLD(pswitch_off_png)

// struct definition to read binary data into cairo surface 
typedef struct  {
	const unsigned char * data;
	long int position;
} binary_stream;

// define controller type
typedef enum {
	KNOB,
	SWITCH,
	BSWITCH,
	ENUM,
} ctype;

// define controller position in window
typedef struct {
	int x;
	int y;
	int width;
	int height;
} gx_alinment;

// define controller adjustment
typedef struct {
	float std_value;
	float value;
	float min_value;
	float max_value;
	float step;
} gx_adjustment;

// controller struct
typedef struct {
	gx_adjustment adj;
	gx_alinment al;
	bool is_active;
	const char* label;
	ctype type;
	PortIndex port;
} gx_controller;

// resize window
typedef struct {
	double x;
	double y;
	double x1;
	double y1;
	double x2;
	double y2;
	double c;
	double xc;
} gx_scale;

// main window struct
typedef struct {
	Display *dpy;
	Window win;
	void *parentXwindow;
	Visual *visual;
	long event_mask;

	int width;
	int height;
	int init_width;
	int init_height;
	int pos_x;
	int pos_y;

	binary_stream png_stream;
	cairo_surface_t *pedal;
	cairo_surface_t *pswitch_on;
	cairo_surface_t *pswitch_off;
	cairo_surface_t *frame;
	cairo_t *crf;
	cairo_t *cr;
	cairo_surface_t *surface;

	gx_controller controls[CONTROLS];
	gx_scale rescale;

	void *controller;
	LV2UI_Write_Function write_function;
	LV2UI_Resize* resize;
} gx_valvecasterUI;

// forward declaration to resize window and cairo surface
static void resize_event(gx_valvecasterUI *ui);

// read png data from binary blob
cairo_status_t png_stream_reader (void *_stream, unsigned char *data, unsigned int length) {
	binary_stream * stream = (binary_stream *) _stream;
	unsigned int i;
	for (i = 0; i < length; i++) {
		data[i] = stream->data[stream->position++];
	}
	return CAIRO_STATUS_SUCCESS;
}

// init the xwindow and return the LV2UI handle
static LV2UI_Handle instantiate(const struct _LV2UI_Descriptor * descriptor,
			const char * plugin_uri, const char * bundle_path,
			LV2UI_Write_Function write_function,
			LV2UI_Controller controller, LV2UI_Widget * widget,
			const LV2_Feature * const * features) {

	gx_valvecasterUI* ui = (gx_valvecasterUI*)malloc(sizeof(gx_valvecasterUI));

	if (!ui) {
		fprintf(stderr,"ERROR: failed to instantiate plugin with URI %s\n", plugin_uri);
		return NULL;
	}

	ui->parentXwindow = 0;
	LV2UI_Resize* resize = NULL;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_UI__parent)) {
			ui->parentXwindow = features[i]->data;
		} else if (!strcmp(features[i]->URI, LV2_UI__resize)) {
			resize = (LV2UI_Resize*)features[i]->data;
		}
	}

	if (ui->parentXwindow == NULL)  {
		fprintf(stderr, "ERROR: Failed to open parentXwindow for %s\n", plugin_uri);
		free(ui);
		return NULL;
	}

	ui->dpy = XOpenDisplay(0);

	if (ui->dpy == NULL)  {
		fprintf(stderr, "ERROR: Failed to open display for %s\n", plugin_uri);
		free(ui);
		return NULL;
	}

	ui->controls[0] = { 1.0, 1.0, 0.0, 1.0, 1.0, 40, 30, 61, 61, false,"BYPASS", BSWITCH, BYPASS};
CONTS

	ui->png_stream.data = LDVAR(pedal_png);
	ui->png_stream.position = 0;
	ui->pedal = cairo_image_surface_create_from_png_stream(&png_stream_reader, (void *)&ui->png_stream);
	ui->init_width = cairo_image_surface_get_width(ui->pedal);
	ui->height = ui->init_height = cairo_image_surface_get_height(ui->pedal);
	ui->width = ui->init_width -140 + (70 * CONTROLS);

	ui->win = XCreateWindow(ui->dpy, (Window)ui->parentXwindow, 0, 0,
								ui->width, ui->height, 0,
								CopyFromParent, InputOutput,
								CopyFromParent, CopyFromParent, 0);

	ui->event_mask = StructureNotifyMask|ExposureMask
					|ButtonPressMask|Button1MotionMask;

	XSelectInput(ui->dpy, ui->win, ui->event_mask);
	XMapWindow(ui->dpy, ui->win);
	XClearWindow(ui->dpy, ui->win);

	ui->visual = DefaultVisual(ui->dpy, DefaultScreen (ui->dpy));
	ui->surface = cairo_xlib_surface_create (ui->dpy, ui->win, ui->visual,
										ui->width, ui->height);
	ui->cr = cairo_create(ui->surface);

	ui->frame = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 61, 81);
	ui->crf = cairo_create (ui->frame);

	ui->png_stream.data = LDVAR(pswitch_on_png);
	ui->png_stream.position = 0;
	ui->pswitch_on = cairo_image_surface_create_from_png_stream(&png_stream_reader, (void *)&ui->png_stream);

	ui->png_stream.data = LDVAR(pswitch_off_png);
	ui->png_stream.position = 0;
	ui->pswitch_off = cairo_image_surface_create_from_png_stream(&png_stream_reader, (void *)&ui->png_stream);

	*widget = (void*)ui->win;

	if (resize){
		ui->resize = resize;
		resize->ui_resize(resize->handle, ui->width, ui->height);
	}

	ui->rescale.x  = (double)ui->width/ui->init_width;
	ui->rescale.y  = (double)ui->height/ui->init_height;
	ui->rescale.x1 = (double)ui->init_width/ui->width;
	ui->rescale.y1 = (double)ui->init_height/ui->height;
	ui->rescale.xc = (double)ui->width/(ui->init_width-140 + (70 * CONTROLS));
	ui->rescale.c = (ui->rescale.xc < ui->rescale.y) ? ui->rescale.xc : ui->rescale.y;
	ui->rescale.x2 =  ui->rescale.xc / ui->rescale.c;
	ui->rescale.y2 = ui->rescale.y / ui->rescale.c;

	ui->controller = controller;
	ui->write_function = write_function;
	resize_event(ui);

	return (LV2UI_Handle)ui;
}

// cleanup after usage
static void cleanup(LV2UI_Handle handle) {
	gx_valvecasterUI* ui = (gx_valvecasterUI*)handle;
	cairo_destroy(ui->cr);
	cairo_destroy(ui->crf);
	cairo_surface_destroy(ui->pedal);
	cairo_surface_destroy(ui->pswitch_on);
	cairo_surface_destroy(ui->pswitch_off);
	cairo_surface_destroy(ui->surface);
	cairo_surface_destroy(ui->frame);
	XDestroyWindow(ui->dpy, ui->win);
	XCloseDisplay(ui->dpy);
	free(ui);
}

// draw knobs and simple switches
static void knob_expose(gx_valvecasterUI *ui,gx_controller* knob) {
	cairo_set_operator(ui->crf,CAIRO_OPERATOR_CLEAR);
	cairo_paint(ui->crf);
	cairo_set_operator(ui->crf,CAIRO_OPERATOR_OVER);
	static const double scale_zero = 20 * (M_PI/180); // defines "dead zone" for knobs
	int arc_offset = 0;
	int knob_x = 0;
	int knob_y = 0;
	int w = cairo_image_surface_get_width(ui->frame);
	int h = cairo_image_surface_get_height(ui->frame)-20;
	int grow = (w > h) ? h:w;
	if (knob->type == SWITCH) {
		knob_x = grow-20;
		knob_y = grow-20; 
	} else {
		knob_x = grow-1;
		knob_y = grow-1;
	}
	/** get values for the knob **/

	int knobx = (w - knob_x) * 0.5;
	int knobx1 = w* 0.5;

	int knoby = (h - knob_y) * 0.5;
	int knoby1 = h * 0.5;

	double knobstate = (knob->adj.value - knob->adj.min_value) / (knob->adj.max_value - knob->adj.min_value);
	double angle = scale_zero + knobstate * 2 * (M_PI - scale_zero);

	double pointer_off =knob_x/6;
	double radius = min(knob_x-pointer_off, knob_y-pointer_off) / 2;
	double lengh_x = (knobx+radius+pointer_off/2) - radius * sin(angle);
	double lengh_y = (knoby+radius+pointer_off/2) + radius * cos(angle);
	double radius_x = (knobx+radius+pointer_off/2) - radius/ 1.18 * sin(angle);
	double radius_y = (knoby+radius+pointer_off/2) + radius/ 1.18 * cos(angle);

	cairo_arc(ui->crf,knobx1+arc_offset, knoby1+arc_offset, knob_x/2.1, 0, 2 * M_PI );
	cairo_pattern_t*pat =
	cairo_pattern_create_radial (knobx1+arc_offset-knob_x/6,knoby1+arc_offset-knob_x/6, 1,knobx1+arc_offset,knoby1+arc_offset,knob_x/2.1 );
	cairo_pattern_add_color_stop_rgb (pat, 0.5, 0.15, 0.15, 0.15);
	cairo_pattern_add_color_stop_rgb (pat, 0.7, 0.2, 0.2, 0.2);
	cairo_pattern_add_color_stop_rgb (pat, 0.9, 0.1, 0.1,0.1);
	cairo_pattern_add_color_stop_rgb (pat, 1, 0.1, 0.1,0.1);

	cairo_set_source (ui->crf, pat);
	cairo_fill_preserve (ui->crf);
	cairo_set_source_rgb (ui->crf, 0., 0., 0.);
	cairo_set_line_width(ui->crf, 1.0);
	cairo_stroke(ui->crf);
	cairo_new_path (ui->crf);

	cairo_arc(ui->crf,knobx1+arc_offset, knoby1+arc_offset, knob_x/3.1, 0, 2 * M_PI );
	pat =
	cairo_pattern_create_radial (knobx1+arc_offset-knob_x/6,knoby1+arc_offset-knob_x/6, 1,knobx1+arc_offset,knoby1+arc_offset,knob_x/2.1 );
	if (knob->type == SWITCH) {
		if (knobstate) {
			cairo_pattern_add_color_stop_rgb (pat, 0, 0.2, 0.2, 0.2);
			cairo_pattern_add_color_stop_rgb (pat, 0.7, 0.3, 0.3, 0.3);
			cairo_pattern_add_color_stop_rgb (pat, 1, 0.2, 0.2,0.2);  
		} else {
			cairo_pattern_add_color_stop_rgb (pat, 0, 0.1, 0.1, 0.1);
			cairo_pattern_add_color_stop_rgb (pat, 0.7, 0.2, 0.2, 0.2);
			cairo_pattern_add_color_stop_rgb (pat, 1, 0.1, 0.1,0.1); 
		}
	} else {
		cairo_pattern_add_color_stop_rgb (pat, 0, 0.1, 0.1, 0.1);
		cairo_pattern_add_color_stop_rgb (pat, 0.7, 0.2, 0.2, 0.2);
		cairo_pattern_add_color_stop_rgb (pat, 1, 0.1, 0.1,0.1);
	}

	cairo_set_source (ui->crf, pat);
	cairo_fill_preserve (ui->crf);
	cairo_set_source_rgb (ui->crf, 0.1, 0.1, 0.1);
	cairo_set_line_width(ui->crf, 4.0);
	cairo_stroke_preserve(ui->crf);
	cairo_new_path (ui->crf);
	cairo_arc(ui->crf,knobx1+arc_offset, knoby1+arc_offset, knob_x/3.1, 0, 2 * M_PI );
	cairo_set_source_rgb (ui->crf, 0., 0., 0.);
	cairo_set_line_width(ui->crf, 1.0);
	cairo_stroke(ui->crf);
	cairo_new_path (ui->crf);

	/** create a rotating pointer on the kob**/
	cairo_set_line_cap(ui->crf, CAIRO_LINE_CAP_ROUND); 
	cairo_set_line_join(ui->crf, CAIRO_LINE_JOIN_BEVEL);
	cairo_move_to(ui->crf, radius_x, radius_y);
	cairo_line_to(ui->crf,lengh_x,lengh_y);
	cairo_set_line_width(ui->crf,min(5, max(2,knob_x/30)));
	cairo_set_source_rgb (ui->crf,0.63,0.63,0.63);
	cairo_stroke(ui->crf);
	cairo_new_path (ui->crf);

	cairo_text_extents_t extents;
	/** show value on the kob**/
	if (knob->type == KNOB) {
		char s[64];
		const char* format[] = {"%.1f", "%.2f", "%.3f"};
		snprintf(s, 63, format[2-1], knob->adj.value);
		cairo_set_source_rgba (ui->crf, 0.6, 0.6, 0.6,0.6);
		cairo_set_font_size (ui->crf, 11.0);
		cairo_select_font_face (ui->crf, "Sans", CAIRO_FONT_SLANT_NORMAL,
								   CAIRO_FONT_WEIGHT_BOLD);
		cairo_text_extents(ui->crf, "0.00", &extents);
		cairo_move_to (ui->crf, knobx1-extents.width/2, knoby1+extents.height/2);
		cairo_show_text(ui->crf, s);
		cairo_new_path (ui->crf);
	} else if (knob->type == SWITCH) {
		cairo_set_source_rgba (ui->crf, 0.0, 0.0, 0.0,1.0);
		cairo_text_extents(ui->crf,"Off", &extents);
		cairo_move_to (ui->crf, knobx1-knob_x/2.4-extents.width/1.6, knoby1+knob_y/2+extents.height/1.4);
		cairo_show_text(ui->crf, "Off");
		cairo_new_path (ui->crf);

		cairo_text_extents(ui->crf,"On", &extents);
		cairo_move_to (ui->crf, knobx1+knob_x/2.6-extents.width/2.3, knoby1+knob_y/2+extents.height/1.4);
		cairo_show_text(ui->crf, "On");
		cairo_new_path (ui->crf);
	}
	cairo_pattern_destroy (pat);

	/** show label below the knob**/
	cairo_set_source_rgba (ui->crf, 0.0, 0.0, 0.0,1.0);
	cairo_set_font_size (ui->crf, 12.0);
	cairo_select_font_face (ui->crf, "Sans", CAIRO_FONT_SLANT_NORMAL,
							   CAIRO_FONT_WEIGHT_BOLD);
	cairo_text_extents(ui->crf,knob->label , &extents);

	cairo_move_to (ui->crf, knobx1-extents.width/2, grow+6+extents.height);
	cairo_show_text(ui->crf, knob->label);
	cairo_new_path (ui->crf);
}

// draw the power switch (bypass)
static void bypass_expose(gx_valvecasterUI *ui, gx_controller* switch_) {
	cairo_set_operator(ui->crf,CAIRO_OPERATOR_CLEAR);
	cairo_paint(ui->crf);
	cairo_set_operator(ui->crf,CAIRO_OPERATOR_OVER);
	if(switch_->adj.value > 0.9)
		cairo_set_source_surface (ui->crf, ui->pswitch_on, 0, 0);
	else
		cairo_set_source_surface (ui->crf, ui->pswitch_off, 0, 0);
	cairo_paint (ui->crf);
	/** show label below the switch**/
	cairo_text_extents_t extents;
	cairo_set_source_rgba (ui->crf, 0.0, 0.0, 0.0,1.0);
	cairo_set_font_size (ui->crf, 12.0);
	cairo_select_font_face (ui->crf, "Sans", CAIRO_FONT_SLANT_NORMAL,
							   CAIRO_FONT_WEIGHT_BOLD);
	cairo_text_extents(ui->crf,switch_->label , &extents);

	cairo_move_to (ui->crf, 30.0-extents.width/2, 67.0+extents.height);
	cairo_show_text(ui->crf, switch_->label);
	cairo_new_path (ui->crf);
}

// select draw methode by controller type
static void draw_controller(gx_valvecasterUI *ui, gx_controller* controller) {
	if (controller->type == KNOB) knob_expose(ui, controller);
	else if (controller->type == SWITCH) knob_expose(ui, controller);
	else if (controller->type == BSWITCH) bypass_expose(ui, controller);
}

// general expose callback, 
static void _expose(gx_valvecasterUI *ui) {
	static const char* plug_name = "GxEffectNAME" ;
	cairo_push_group (ui->cr);

	cairo_scale (ui->cr, ui->rescale.x, ui->rescale.y);

	cairo_set_source_surface (ui->cr, ui->pedal, 0, 0);
	cairo_paint (ui->cr);

	cairo_text_extents_t extents;
	cairo_set_source_rgba (ui->cr, 0.6, 0.6, 0.6,1.0);
	cairo_set_font_size (ui->cr, 25.0);
	cairo_select_font_face (ui->cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
							   CAIRO_FONT_WEIGHT_BOLD);
	cairo_text_extents(ui->cr, plug_name, &extents);
	cairo_move_to (ui->cr, ((double)(ui->width/2.0)/ui->rescale.x-(extents.width)/2.0),
	  (double)(ui->height-20.0)/ui->rescale.y-extents.height);
	cairo_show_text(ui->cr, plug_name);

	cairo_scale (ui->cr, ui->rescale.x1, ui->rescale.y1);
	cairo_scale (ui->cr, ui->rescale.c, ui->rescale.c);

	for (int i=0;i<CONTROLS;i++) {
		draw_controller(ui, &ui->controls[i]);
		cairo_set_source_surface (ui->cr, ui->frame, 
		  ((double)ui->controls[i].al.x * ui->rescale.x2,
		  (double)ui->controls[i].al.y * ui->rescale.y2);
		cairo_paint (ui->cr);
	}

	cairo_pop_group_to_source (ui->cr);
	cairo_paint (ui->cr);
}

// redraw a single controller
static void controller_expose(gx_valvecasterUI *ui, gx_controller * control) {
	cairo_push_group (ui->cr);
	cairo_scale (ui->cr, ui->rescale.x, ui->rescale.y);

	cairo_set_source_surface (ui->cr, ui->pedal, 0, 0);

	cairo_scale (ui->cr, ui->rescale.x1, ui->rescale.y1);
	cairo_scale (ui->cr, ui->rescale.c, ui->rescale.c);
	cairo_rectangle (ui->cr,(double)control->al.x * ui->rescale.x2,
	  (double)control->al.y * ui->rescale.y2,
	  (double)control->al.width, (double)control->al.height+20.0);
	cairo_fill(ui->cr);
	cairo_stroke(ui->cr);

	draw_controller(ui, control);
	cairo_set_source_surface (ui->cr, ui->frame, 
	  (double)control->al.x * ui->rescale.x2,
	  (double)control->al.y * ui->rescale.y2);
	cairo_paint (ui->cr);

	cairo_pop_group_to_source (ui->cr);
	cairo_paint (ui->cr);
}

// recive port changes from host
static void port_event(LV2UI_Handle handle, uint32_t port_index,
						uint32_t buffer_size, uint32_t format,
						const void * buffer) {
	gx_valvecasterUI* ui = (gx_valvecasterUI*)handle;
	float value = *(float*)buffer;
	for (int i=0;i<CONTROLS;i++) {
		if (port_index == ui->controls[i].port) {
			if (fabs(value - ui->controls[i].adj.value)>=0.00001) {
				ui->controls[i].adj.value = value;
				controller_expose(ui, &ui->controls[i]);
			}
		}
	}
}

// check which controller is active
bool aligned(int x, int y, gx_controller *control, gx_valvecasterUI *ui) {
	double ax = control->al.x * ui->rescale.x2 * ui->rescale.c;
	double ay = control->al.y * ui->rescale.y2 * ui->rescale.c;
	double aw = ax + (control->al.width * ui->rescale.c);
	double ah = ay + (control->al.height * ui->rescale.c);
	return ((x >= ax ) && (x <= aw)
	  && (y >= ay ) && (y <= ah)) ? true : false;
}

// resize the xwindow and the cairo xlib surface
static void resize_event(gx_valvecasterUI *ui) {
	XWindowAttributes attrs;
	XGetWindowAttributes(ui->dpy, (Window)ui->parentXwindow, &attrs);
	ui->width = attrs.width;
	ui->height = attrs.height;
	XMoveResizeWindow (ui->dpy,ui->win , 0, 0, ui->width, ui->height);
	cairo_xlib_surface_set_size( ui->surface, ui->width, ui->height);
	ui->rescale.x  = (double)ui->width/ui->init_width;
	ui->rescale.y  = (double)ui->height/ui->init_height;
	ui->rescale.x1 = (double)ui->init_width/ui->width;
	ui->rescale.y1 = (double)ui->init_height/ui->height;
	ui->rescale.xc = (double)ui->width/(ui->init_width-140 + (70 * CONTROLS));
	ui->rescale.c = (ui->rescale.xc < ui->rescale.y) ? ui->rescale.xc : ui->rescale.y;
	ui->rescale.x2 =  ui->rescale.xc / ui->rescale.c;
	ui->rescale.y2 = ui->rescale.y / ui->rescale.c;
}

// mouse wheel scroll event
static void scroll_event(gx_valvecasterUI *ui, int direction) {
	float value;
	for (int i=0;i<CONTROLS;i++) {
		if (aligned(ui->pos_x, ui->pos_y, &ui->controls[i], ui)) {
			value = min(ui->controls[i].adj.max_value,max(ui->controls[i].adj.min_value, 
			  ui->controls[i].adj.value + (ui->controls[i].adj.step * direction)));
			if(fabs(value - ui->controls[i].adj.value)>=0.00001) {
				ui->controls[i].adj.value = value;
				ui->write_function(ui->controller,ui->controls[i].port,sizeof(float),0,&value);
				controller_expose(ui, &ui->controls[i]);
			}
		}
		ui->controls[i].is_active = false;
	}
}

// controll is switch, so switch value
static void switch_event(gx_valvecasterUI *ui, gx_controller* control) {
	float value = control->adj.value ? 0.0 : 1.0;
	if(fabs(value - control->adj.value)>=0.00001) {
		control->adj.value = value; 
		ui->write_function(ui->controller,control->port,sizeof(float),0,&value);
		controller_expose(ui, control);
	}
}

// left mouse button is pressed, generate a switch event, or set controller active
static void button1_event(gx_valvecasterUI *ui, double* start_value) {
	for (int i=0;i<CONTROLS;i++) {
		if (aligned(ui->pos_x, ui->pos_y, &ui->controls[i], ui)) {
			if (ui->controls[i].type == BSWITCH ||ui->controls[i].type == SWITCH) {
				switch_event(ui, &ui->controls[i]);
			} else {
				ui->controls[i].is_active = true;
				*(start_value) = ui->controls[i].adj.value;
			}
		} else {
			ui->controls[i].is_active = false;
		}
	}
}

// set min std or max value, depending on which key is pressed
static void set_key_value(gx_valvecasterUI *ui, int set_value) {
	float value = 0.0;
	for (int i=0;i<CONTROLS;i++) {
		if (ui->controls[i].is_active) {
			if (set_value == 1) value = ui->controls[i].adj.min_value;
			else if (set_value == 2) value = ui->controls[i].adj.std_value;
			else if (set_value == 3) value = ui->controls[i].adj.max_value;
			if(fabs(value - ui->controls[i].adj.value)>=0.00001) {
				ui->controls[i].adj.value = value;
				ui->write_function(ui->controller,ui->controls[i].port,sizeof(float),0,&value);
				controller_expose(ui, &ui->controls[i]);
			}
		}
	}
}

// scroll up/down on key's up/right down/left
static void key_event(gx_valvecasterUI *ui, int direction) {
	float value;
	for (int i=0;i<CONTROLS;i++) {
		if (ui->controls[i].is_active) {
			value = min(ui->controls[i].adj.max_value,max(ui->controls[i].adj.min_value, 
			  ui->controls[i].adj.value + (ui->controls[i].adj.step * direction)));
			if(fabs(value - ui->controls[i].adj.value)>=0.00001) {
				ui->controls[i].adj.value = value;
				ui->write_function(ui->controller,ui->controls[i].port,sizeof(float),0,&value);
				controller_expose(ui, &ui->controls[i]);
			}
		}
	}
}

// set previous controller active on shift+tab key's
static void set_previous_controller_active(gx_valvecasterUI *ui) {
	for (int i=0;i<CONTROLS;i++) {
		if(ui->controls[i].is_active == true) {
			ui->controls[i].is_active = false;
			if(i>0) {
				ui->controls[i-1].is_active = true;
				return;
			} else {
				ui->controls[CONTROLS-1].is_active = true;
				return;
			}
		}
	}
	ui->controls[CONTROLS-1].is_active = true;
}

// set next controller active on tab key
static void set_next_controller_active(gx_valvecasterUI *ui) {
	for (int i=0;i<CONTROLS;i++) {
		if(ui->controls[i].is_active == true) {
			ui->controls[i].is_active = false;
			if(i<CONTROLS-1) {
				ui->controls[i+1].is_active = true;
				return;
			} else {
				ui->controls[0].is_active = true;
				return;
			}
		}
	}
	ui->controls[0].is_active = true;
}

// mouse move while left button is pressed
static void motion_event(gx_valvecasterUI *ui, double start_value, XMotionEvent *mo) {
	static const double scaling = 0.5;
	float value = 0.0;
	for (int i=1;i<CONTROLS;i++) {
		if (ui->controls[i].is_active) {
			value = min(ui->controls[i].adj.max_value,max(ui->controls[i].adj.min_value,start_value + 
			  (((double)(ui->pos_y - mo->y)*scaling*ui->controls[i].adj.step)*
			  (ui->controls[i].adj.max_value-ui->controls[i].adj.min_value))));
			if(fabs(value - ui->controls[i].adj.value)>=0.00001) {
				ui->controls[i].adj.value = value;
				ui->write_function(ui->controller,ui->controls[i].port,sizeof(float),0,&value);
				controller_expose(ui, &ui->controls[i]);
			}
		}
	}
	
}

// general xevent handler
static void event_handler(gx_valvecasterUI *ui) {
	XEvent xev;
	static double start_value = 0.0;

	while (XPending(ui->dpy) > 0) {
		XNextEvent(ui->dpy, &xev);

		switch(xev.type) {
			case ConfigureNotify:
				// configure event, we only check for resize events here
				resize_event(ui);
			break;
			case Expose: 
				// only fetch the last expose event
				if (xev.xexpose.count == 0) 
					_expose(ui);
			break;

			case ButtonPress:
				ui->pos_x = xev.xbutton.x;
				ui->pos_y = xev.xbutton.y;

				switch(xev.xbutton.button) {
					case Button1:
						// left mouse button click
						button1_event(ui, &start_value);
					break;
					case Button2:
						// click on the mouse wheel
						//fprintf(stderr,"Button2 \n");
					break;
					case Button3:
						// right mouse button click
						//fprintf(stderr,"Button3 \n");
					break;
					case  Button4:
						// mouse wheel scroll up
						scroll_event(ui, 1);
					break;
					case Button5:
						// mouse wheel scroll down
						scroll_event(ui, -1);
					break;
					default:
					break;
				}

			case KeyPress:
				if ((xev.xkey.state == ShiftMask) &&
				  (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Tab)))
					set_previous_controller_active(ui);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Tab))
					set_next_controller_active(ui);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Up))
					key_event(ui, 1);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Right))
					key_event(ui, 1);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Down))
					key_event(ui, -1);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Left))
					key_event(ui, -1);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Home))
					set_key_value(ui, 1);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_Insert))
					set_key_value(ui, 2);
				else if (xev.xkey.keycode == XKeysymToKeycode(ui->dpy,XK_End))
					set_key_value(ui, 3);
			break;

			case MotionNotify:
				// mouse move while button1 is pressed
				if(xev.xmotion.state == Button1MotionMask) {
					motion_event(ui, start_value, &xev.xmotion);
				}
			break;
			default:
			break;
		}
	}
}

// LV2 interface to host
static int ui_idle(LV2UI_Handle handle) {
	gx_valvecasterUI* ui = (gx_valvecasterUI*)handle;
	event_handler(ui);
	return 0;
}

static const void* extension_data(const char* uri) {
	static const LV2UI_Idle_Interface idle = { ui_idle };
	if (!strcmp(uri, LV2_UI__idleInterface)) {
		return &idle;
	}
	return NULL;
}

static const LV2UI_Descriptor descriptor = {
	GXPLUGIN_UI_URI,
	instantiate,
	cleanup,
	port_event,
	extension_data
};

extern "C"
LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
	switch (index) {
		case 0:
			return &descriptor;
		default:
		return NULL;
	}
}

