#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>

#include <cutils/properties.h>

#include <GLES3/gl3.h>
#include <EGL/eglext.h>

#include <json.h>
#include <czmq.h>

#include "nanovg.h"
#define NANOVG_GLES3_IMPLEMENTATION
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

#include "common/timing.h"
#include "common/util.h"
#include "common/swaglog.h"
#include "common/mat.h"
#include "common/glutil.h"

#include "common/touch.h"
#include "common/framebuffer.h"
#include "common/visionipc.h"
#include "common/modeldata.h"
#include "common/params.h"

#include "cereal/gen/c/log.capnp.h"

// Calibration status values from controlsd.py
#define CALIBRATION_UNCALIBRATED 0
#define CALIBRATION_CALIBRATED 1
#define CALIBRATION_INVALID 2

#define STATUS_STOPPED 0
#define STATUS_DISENGAGED 1
#define STATUS_ENGAGED 2
#define STATUS_WARNING 3
#define STATUS_ALERT 4
#define STATUS_MAX 5

#define UI_BUF_COUNT 4


const int box_x = 330;
const int box_y = 30;
const int box_width = 1560;
const int box_height = 1020;

const uint8_t bg_colors[][4] = {
  [STATUS_STOPPED] = {0x07, 0x23, 0x39, 0xff},
  [STATUS_DISENGAGED] = {0x17, 0x33, 0x49, 0xff},
  [STATUS_ENGAGED] = {0x17, 0x86, 0x44, 0xff},
  [STATUS_WARNING] = {0xDA, 0x6F, 0x25, 0xff},
  [STATUS_ALERT] = {0xC9, 0x22, 0x31, 0xff},
};

const uint8_t alert_colors[][4] = {
  [STATUS_STOPPED] = {0x07, 0x23, 0x39, 0x80},
  [STATUS_DISENGAGED] = {0x17, 0x33, 0x49, 0x80},
  [STATUS_ENGAGED] = {0x17, 0x86, 0x44, 0x80},
  [STATUS_WARNING] = {0xDA, 0x6F, 0x25, 0x80},
  [STATUS_ALERT] = {0xC9, 0x22, 0x31, 0x80},
};

typedef struct UIScene {
  int frontview;

  uint8_t *bgr_ptr;

  int transformed_width, transformed_height;

  uint64_t model_ts;
  ModelData model;

  float mpc_x[50];
  float mpc_y[50];

  bool world_objects_visible;
  mat3 warp_matrix;           // transformed box -> frame.
  mat4 extrinsic_matrix;      // Last row is 0 so we can use mat4.

  float v_cruise;
  float v_ego;
  float curvature;
  int engaged;

  int lead_status;
  float lead_d_rel, lead_y_rel, lead_v_rel;

  uint8_t *bgr_front_ptr;
  int front_box_x, front_box_y, front_box_width, front_box_height;

  uint64_t alert_ts;
  char alert_text1[1024];
  char alert_text2[1024];

  float awareness_status;

  uint64_t started_ts;

  // Used to display calibration progress
  int cal_status;
  int cal_perc;
} UIScene;

typedef struct UIState {
  pthread_mutex_t lock;
  pthread_cond_t bg_cond;

  FramebufferState *fb;
  int fb_w, fb_h;
  EGLDisplay display;
  EGLSurface surface;

  NVGcontext *vg;

  int font_courbd;
  int font_sans_regular;
  int font_sans_semibold;

  zsock_t *thermal_sock;
  void *thermal_sock_raw;
  zsock_t *model_sock;
  void *model_sock_raw;
  zsock_t *live100_sock;
  void *live100_sock_raw;
  zsock_t *livecalibration_sock;
  void *livecalibration_sock_raw;
  zsock_t *live20_sock;
  void *live20_sock_raw;
  zsock_t *livempc_sock;
  void *livempc_sock_raw;
  zsock_t *plus_sock;
  void *plus_sock_raw;

  int plus_state;

  // vision state
  bool vision_connected;
  bool vision_connect_firstrun;
  int ipc_fd;

  VIPCBuf bufs[UI_BUF_COUNT];
  VIPCBuf front_bufs[UI_BUF_COUNT];
  int cur_vision_idx;
  int cur_vision_front_idx;

  GLuint frame_program;

  GLuint frame_tex;
  GLint frame_pos_loc, frame_texcoord_loc;
  GLint frame_texture_loc, frame_transform_loc;

  GLuint line_program;
  GLint line_pos_loc, line_color_loc;
  GLint line_transform_loc;

  unsigned int rgb_width, rgb_height;
  mat4 rgb_transform;

  unsigned int rgb_front_width, rgb_front_height;
  GLuint frame_front_tex;

  bool intrinsic_matrix_loaded;
  mat3 intrinsic_matrix;

  UIScene scene;

  bool awake;
  int awake_timeout;

  int status;
  bool is_metric;
  bool passive;

  float light_sensor;
} UIState;

static int last_brightness = -1;
static void set_brightness(int brightness) {
  if (last_brightness != brightness) {
    //printf("setting brightness %d\n", brightness);
    // can't hurt
    FILE *f = fopen("/sys/class/leds/lcd-backlight/brightness", "wb");
    if (f != NULL) {
      fprintf(f, "%d", brightness);
      fclose(f);
      last_brightness = brightness;
    }
  }
}

static void set_awake(UIState *s, bool awake) {
  if (awake) {
    // 30 second timeout at 30 fps
    s->awake_timeout = 30*30;
  }
  if (s->awake != awake) {
    s->awake = awake;

    if (awake) {
      LOG("awake normal");
      framebuffer_set_power(s->fb, HWC_POWER_MODE_NORMAL);
    } else {
      LOG("awake off");
      framebuffer_set_power(s->fb, HWC_POWER_MODE_OFF);
    }
  }
}

volatile int do_exit = 0;
static void set_do_exit(int sig) {
  do_exit = 1;
}


static const char frame_vertex_shader[] =
  "attribute vec4 aPosition;\n"
  "attribute vec4 aTexCoord;\n"
  "uniform mat4 uTransform;\n"
  "varying vec4 vTexCoord;\n"
  "void main() {\n"
  "  gl_Position = uTransform * aPosition;\n"
  "  vTexCoord = aTexCoord;\n"
  "}\n";

static const char frame_fragment_shader[] =
  "precision mediump float;\n"
  "uniform sampler2D uTexture;\n"
  "varying vec4 vTexCoord;\n"
  "void main() {\n"
  "  gl_FragColor = texture2D(uTexture, vTexCoord.xy);\n"
  "}\n";

static const char line_vertex_shader[] =
  "attribute vec4 aPosition;\n"
  "attribute vec4 aColor;\n"
  "uniform mat4 uTransform;\n"
  "varying vec4 vColor;\n"
  "void main() {\n"
  "  gl_Position = uTransform * aPosition;\n"
  "  vColor = aColor;\n"
  "}\n";

static const char line_fragment_shader[] =
  "precision mediump float;\n"
  "uniform sampler2D uTexture;\n"
  "varying vec4 vColor;\n"
  "void main() {\n"
  "  gl_FragColor = vColor;\n"
  "}\n";


static const mat4 device_transform = {{
  1.0,  0.0, 0.0, 0.0,
  0.0,  1.0, 0.0, 0.0,
  0.0,  0.0, 1.0, 0.0,
  0.0,  0.0, 0.0, 1.0,
}};

// frame from 4/3 to box size with a 2x zoon
static const mat4 frame_transform = {{
  2*(4./3.)/((float)box_width/box_height), 0.0, 0.0, 0.0,
                                           0.0, 2.0, 0.0, 0.0,
                                           0.0, 0.0, 1.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0,
}};

static void ui_init(UIState *s) {
  memset(s, 0, sizeof(UIState));

  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->bg_cond, NULL);

  // init connections

  s->thermal_sock = zsock_new_sub(">tcp://127.0.0.1:8005", "");
  assert(s->thermal_sock);
  s->thermal_sock_raw = zsock_resolve(s->thermal_sock);

  s->model_sock = zsock_new_sub(">tcp://127.0.0.1:8009", "");
  assert(s->model_sock);
  s->model_sock_raw = zsock_resolve(s->model_sock);

  s->live100_sock = zsock_new_sub(">tcp://127.0.0.1:8007", "");
  assert(s->live100_sock);
  s->live100_sock_raw = zsock_resolve(s->live100_sock);

  s->livecalibration_sock = zsock_new_sub(">tcp://127.0.0.1:8019", "");
  assert(s->livecalibration_sock);
  s->livecalibration_sock_raw = zsock_resolve(s->livecalibration_sock);

  s->live20_sock = zsock_new_sub(">tcp://127.0.0.1:8012", "");
  assert(s->live20_sock);
  s->live20_sock_raw = zsock_resolve(s->live20_sock);

  s->livempc_sock = zsock_new_sub(">tcp://127.0.0.1:8035", "");
  assert(s->livempc_sock);
  s->livempc_sock_raw = zsock_resolve(s->livempc_sock);

  s->plus_sock = zsock_new_sub(">tcp://127.0.0.1:8037", "");
  assert(s->plus_sock);
  s->plus_sock_raw = zsock_resolve(s->plus_sock);

  s->ipc_fd = -1;

  // init display
  s->fb = framebuffer_init("ui", 0x00010000, true,
                           &s->display, &s->surface, &s->fb_w, &s->fb_h);
  assert(s->fb);

  set_awake(s, true);

  // init drawing
  s->vg = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
  assert(s->vg);

  s->font_courbd = nvgCreateFont(s->vg, "courbd", "../assets/courbd.ttf");
  assert(s->font_courbd >= 0);
  s->font_sans_regular = nvgCreateFont(s->vg, "sans-regular", "../assets/OpenSans-Regular.ttf");
  assert(s->font_sans_regular >= 0);
  s->font_sans_semibold = nvgCreateFont(s->vg, "sans-semibold", "../assets/OpenSans-SemiBold.ttf");
  assert(s->font_sans_semibold >= 0);

  // init gl
  s->frame_program = load_program(frame_vertex_shader, frame_fragment_shader);
  assert(s->frame_program);

  s->frame_pos_loc = glGetAttribLocation(s->frame_program, "aPosition");
  s->frame_texcoord_loc = glGetAttribLocation(s->frame_program, "aTexCoord");

  s->frame_texture_loc = glGetUniformLocation(s->frame_program, "uTexture");
  s->frame_transform_loc = glGetUniformLocation(s->frame_program, "uTransform");

  s->line_program = load_program(line_vertex_shader, line_fragment_shader);
  assert(s->line_program);

  s->line_pos_loc = glGetAttribLocation(s->line_program, "aPosition");
  s->line_color_loc = glGetAttribLocation(s->line_program, "aColor");
  s->line_transform_loc = glGetUniformLocation(s->line_program, "uTransform");

  glViewport(0, 0, s->fb_w, s->fb_h);

  glDisable(GL_DEPTH_TEST);

  assert(glGetError() == GL_NO_ERROR);

  {
    char *value;
    const int result = read_db_value(NULL, "Passive", &value, NULL);
    if (result == 0) {
      s->passive = value[0] == '1';
      free(value);
    }
  }
}


// If the intrinsics are in the params entry, this copies them to
// intrinsic_matrix and returns true.  Otherwise returns false.
static bool try_load_intrinsics(mat3 *intrinsic_matrix) {
  char *value;
  const int result = read_db_value(NULL, "CloudCalibration", &value, NULL);

  if (result == 0) {
    JsonNode* calibration_json = json_decode(value);
    free(value);

    JsonNode *intrinsic_json =
        json_find_member(calibration_json, "intrinsic_matrix");

    if (intrinsic_json == NULL || intrinsic_json->tag != JSON_ARRAY) {
      json_delete(calibration_json);
      return false;
    }

    int i = 0;
    JsonNode* json_num;
    json_foreach(json_num, intrinsic_json) {
      intrinsic_matrix->v[i++] = json_num->number_;
    }
    json_delete(calibration_json);

    return true;
  } else {
    return false;
  }
}


static void ui_init_vision(UIState *s, const VisionStreamBufs back_bufs,
                           int num_back_fds, const int *back_fds,
                           const VisionStreamBufs front_bufs, int num_front_fds,
                           const int *front_fds) {
  const VisionUIInfo ui_info = back_bufs.buf_info.ui_info;

  assert(num_back_fds == UI_BUF_COUNT);
  assert(num_front_fds == UI_BUF_COUNT);

  vipc_bufs_load(s->bufs, &back_bufs, num_back_fds, back_fds);
  vipc_bufs_load(s->front_bufs, &front_bufs, num_front_fds, front_fds);

  s->cur_vision_idx = -1;
  s->cur_vision_front_idx = -1;

  s->scene = (UIScene){
      .frontview = 0,
      .cal_status = CALIBRATION_CALIBRATED,
      .transformed_width = ui_info.transformed_width,
      .transformed_height = ui_info.transformed_height,
      .front_box_x = ui_info.front_box_x,
      .front_box_y = ui_info.front_box_y,
      .front_box_width = ui_info.front_box_width,
      .front_box_height = ui_info.front_box_height,
      .world_objects_visible = false,  // Invisible until we receive a calibration message.
  };

  s->rgb_width = back_bufs.width;
  s->rgb_height = back_bufs.height;

  s->rgb_front_width = front_bufs.width;
  s->rgb_front_height = front_bufs.height;

  s->rgb_transform = (mat4){{
    2.0/s->rgb_width, 0.0, 0.0, -1.0,
    0.0, 2.0/s->rgb_height, 0.0, -1.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
  }};

  char *value;
  const int result = read_db_value(NULL, "IsMetric", &value, NULL);
  if (result == 0) {
    s->is_metric = value[0] == '1';
    free(value);
  }
}

static bool ui_alert_active(UIState *s) {
  return (nanos_since_boot() - s->scene.alert_ts) < 20000000000ULL && strlen(s->scene.alert_text1) > 0;
}

static void ui_update_frame(UIState *s) {
  assert(glGetError() == GL_NO_ERROR);

  UIScene *scene = &s->scene;

  if (scene->frontview && scene->bgr_front_ptr) {
    // load front frame texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->frame_front_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->rgb_front_width, s->rgb_front_height,
                    GL_RGB, GL_UNSIGNED_BYTE, scene->bgr_front_ptr);
  } else if (!scene->frontview && scene->bgr_ptr) {
    // load frame texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->frame_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->rgb_width, s->rgb_height,
                    GL_RGB, GL_UNSIGNED_BYTE, scene->bgr_ptr);
  }

  assert(glGetError() == GL_NO_ERROR);
}

static void ui_draw_transformed_box(UIState *s, uint32_t color) {
  const UIScene *scene = &s->scene;

  const mat3 bbt = scene->warp_matrix;

  struct {
    vec3 pos;
    uint32_t color;
  } verts[] = {
    {matvecmul3(bbt, (vec3){{0.0, 0.0, 1.0,}}), color},
    {matvecmul3(bbt, (vec3){{scene->transformed_width, 0.0, 1.0,}}), color},
    {matvecmul3(bbt, (vec3){{scene->transformed_width, scene->transformed_height, 1.0,}}), color},
    {matvecmul3(bbt, (vec3){{0.0, scene->transformed_height, 1.0,}}), color},
    {matvecmul3(bbt, (vec3){{0.0, 0.0, 1.0,}}), color},
  };

  for (int i=0; i<ARRAYSIZE(verts); i++) {
    verts[i].pos.v[0] = verts[i].pos.v[0] / verts[i].pos.v[2];
    verts[i].pos.v[1] = s->rgb_height - verts[i].pos.v[1] / verts[i].pos.v[2];
  }

  glUseProgram(s->line_program);

  mat4 out_mat = matmul(device_transform,
                        matmul(frame_transform, s->rgb_transform));
  glUniformMatrix4fv(s->line_transform_loc, 1, GL_TRUE, out_mat.v);

  glEnableVertexAttribArray(s->line_pos_loc);
  glVertexAttribPointer(s->line_pos_loc, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), &verts[0].pos.v[0]);

  glEnableVertexAttribArray(s->line_color_loc);
  glVertexAttribPointer(s->line_color_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(verts[0]), &verts[0].color);

  assert(glGetError() == GL_NO_ERROR);
  glDrawArrays(GL_LINE_STRIP, 0, ARRAYSIZE(verts));
}

// Projects a point in car to space to the corresponding point in full frame
// image space.
vec3 car_space_to_full_frame(const UIState *s, vec4 car_space_projective) {
  const UIScene *scene = &s->scene;

  // We'll call the car space point p.
  // First project into normalized image coordinates with the extrinsics matrix.
  const vec4 Ep4 = matvecmul(scene->extrinsic_matrix, car_space_projective);

  // The last entry is zero because of how we store E (to use matvecmul).
  const vec3 Ep = {{Ep4.v[0], Ep4.v[1], Ep4.v[2]}};
  const vec3 KEp = matvecmul3(s->intrinsic_matrix, Ep);

  // Project.
  const vec3 p_image = {{KEp.v[0] / KEp.v[2], KEp.v[1] / KEp.v[2], 1.}};
  return p_image;
}


// TODO: refactor with draw_path
static void draw_cross(UIState *s, float x_in, float y_in, float sz, NVGcolor color) {
  const UIScene *scene = &s->scene;

  nvgSave(s->vg);

  // path coords are worked out in rgb-box space
  nvgTranslate(s->vg, 240.0f, 0.0);

  // zooom in 2x
  nvgTranslate(s->vg, -1440.0f / 2, -1080.0f / 2);
  nvgScale(s->vg, 2.0, 2.0);

  nvgScale(s->vg, 1440.0f / s->rgb_width, 1080.0f / s->rgb_height);

  nvgBeginPath(s->vg);
  nvgStrokeColor(s->vg, color);
  nvgStrokeWidth(s->vg, 5);

  const vec4 p_car_space = (vec4){{x_in, y_in, 0., 1.}};
  const vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);

  // scale with distance
  sz *= 20;
  sz /= x_in;
  if (sz > 25) sz = 25;
  if (sz < 10) sz = 10;

  float x = p_full_frame.v[0];
  float y = p_full_frame.v[1];
  if (x >= 0 && y >= 0.) {
    nvgMoveTo(s->vg, x-sz, y);
    nvgLineTo(s->vg, x+sz, y);

    nvgMoveTo(s->vg, x, y-sz);
    nvgLineTo(s->vg, x, y+sz);

    nvgStroke(s->vg);
  }

  nvgRestore(s->vg);
}

static void draw_x_y(UIState *s, const float *x_coords, const float *y_coords, size_t num_points,
                      NVGcolor color) {
  const UIScene *scene = &s->scene;

  nvgSave(s->vg);

  // path coords are worked out in rgb-box space
  nvgTranslate(s->vg, 240.0f, 0.0);

  // zooom in 2x
  nvgTranslate(s->vg, -1440.0f / 2, -1080.0f / 2);
  nvgScale(s->vg, 2.0, 2.0);

  nvgScale(s->vg, 1440.0f / s->rgb_width, 1080.0f / s->rgb_height);

  nvgBeginPath(s->vg);
  nvgStrokeColor(s->vg, color);
  nvgStrokeWidth(s->vg, 2);
  bool started = false;

  for (int i=0; i<num_points; i++) {
    float px = x_coords[i];
    float py = y_coords[i];
    vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);

    float x = p_full_frame.v[0];
    float y = p_full_frame.v[1];
    if (x < 0 || y < 0.) {
      continue;
    }

    if (!started) {
      nvgMoveTo(s->vg, x, y);
      started = true;
    } else {
      nvgLineTo(s->vg, x, y);
    }
  }

  nvgStroke(s->vg);

  nvgRestore(s->vg);
}

static void draw_path(UIState *s, const float *points, float off,
                      NVGcolor color) {
  const UIScene *scene = &s->scene;

  nvgSave(s->vg);

  // path coords are worked out in rgb-box space
  nvgTranslate(s->vg, 240.0f, 0.0);

  // zooom in 2x
  nvgTranslate(s->vg, -1440.0f / 2, -1080.0f / 2);
  nvgScale(s->vg, 2.0, 2.0);

  nvgScale(s->vg, 1440.0f / s->rgb_width, 1080.0f / s->rgb_height);

  nvgBeginPath(s->vg);
  nvgStrokeColor(s->vg, color);
  nvgStrokeWidth(s->vg, 5);
  bool started = false;

  for (int i=0; i<50; i++) {
    float px = (float)i;
    float py = points[i] + off;

    vec4 p_car_space = (vec4){{px, py, 0., 1.}};
    vec3 p_full_frame = car_space_to_full_frame(s, p_car_space);

    float x = p_full_frame.v[0];
    float y = p_full_frame.v[1];
    if (x < 0 || y < 0.) {
      continue;
    }

    if (!started) {
      nvgMoveTo(s->vg, x, y);
      started = true;
    } else {
      nvgLineTo(s->vg, x, y);
    }
  }

  nvgStroke(s->vg);

  nvgRestore(s->vg);
}

static void draw_model_path(UIState *s, const PathData path, NVGcolor color) {
  float var = min(path.std, 0.7);
  draw_path(s, path.points, 0.0, color);
  color.a /= 4;
  draw_path(s, path.points, -var, color);
  draw_path(s, path.points, var, color);
}

static void draw_steering(UIState *s, float curvature) {

  float points[50];
  for (int i = 0; i < 50; i++) {
    float y_actual = i * tan(asin(clamp(i * curvature, -0.999, 0.999)) / 2.);
    points[i] = y_actual;
  }

  draw_path(s, points, 0.0, nvgRGBA(0, 0, 255, 128));
}

static void draw_frame(UIState *s) {
  // draw frame texture
  const UIScene *scene = &s->scene;

  mat4 out_mat;
  float x1, x2, y1, y2;
  if (s->scene.frontview) {
    out_mat = device_transform; // full 16/9

    // flip horizontally so it looks like a mirror
    x2 = (float)scene->front_box_x / s->rgb_front_width;
    x1 = (float)(scene->front_box_x + scene->front_box_width) / s->rgb_front_width;

    y1 = (float)scene->front_box_y / s->rgb_front_height;
    y2 = (float)(scene->front_box_y + scene->front_box_height) / s->rgb_front_height;
  } else {
    out_mat = matmul(device_transform, frame_transform);

    x1 = 0.0;
    x2 = 1.0;
    y1 = 0.0;
    y2 = 1.0;
  }

  const uint8_t frame_indicies[] = {0, 1, 2, 0, 2, 3};
  const float frame_coords[4][4] = {
    {-1.0, -1.0, x2, y1}, //bl
    {-1.0,  1.0, x2, y2}, //tl
    { 1.0,  1.0, x1, y2}, //tr
    { 1.0, -1.0, x1, y1}, //br
  };

  glActiveTexture(GL_TEXTURE0);
  if (s->scene.frontview) {
    glBindTexture(GL_TEXTURE_2D, s->frame_front_tex);
  } else {
    glBindTexture(GL_TEXTURE_2D, s->frame_tex);
  }

  glUseProgram(s->frame_program);

  glUniform1i(s->frame_texture_loc, 0);
  glUniformMatrix4fv(s->frame_transform_loc, 1, GL_TRUE, out_mat.v);

  glEnableVertexAttribArray(s->frame_pos_loc);
  glVertexAttribPointer(s->frame_pos_loc, 2, GL_FLOAT, GL_FALSE,
                        sizeof(frame_coords[0]), frame_coords);

  glEnableVertexAttribArray(s->frame_texcoord_loc);
  glVertexAttribPointer(s->frame_texcoord_loc, 2, GL_FLOAT, GL_FALSE,
                        sizeof(frame_coords[0]), &frame_coords[0][2]);

  assert(glGetError() == GL_NO_ERROR);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, &frame_indicies[0]);
}

/*
 * Draw a rect at specific position with specific dimensions
 */
static void ui_draw_rounded_rect(
    NVGcontext* c,
    int x,
    int y,
    int width,
    int height,
    int radius,
    NVGcolor color
) {

  int bottom_x = x + width;
  int bottom_y = y + height;

  nvgBeginPath(c);

  // Position the rect
  nvgRoundedRect(c, x, y, bottom_x, bottom_y, radius);

  // Color the rect
  nvgFillColor(c, color);

  // Draw the rect
  nvgFill(c);

  // Draw white border around rect
  nvgStrokeColor(c, nvgRGBA(255,255,255,200));
  nvgStroke(c);
}

// Draw all world space objects.
static void ui_draw_world(UIState *s) {
  const UIScene *scene = &s->scene;
  if (!scene->world_objects_visible) {
    return;
  }

  //draw_steering(s, scene->curvature);

  if ((nanos_since_boot() - scene->model_ts) < 1000000000ULL) {
    int left_lane_color = (int)(255 * scene->model.left_lane.prob);
    int right_lane_color = (int)(255 * scene->model.right_lane.prob);
    draw_model_path(
        s, scene->model.left_lane,
        nvgRGBA(left_lane_color, left_lane_color, left_lane_color, 128));
    draw_model_path(
        s, scene->model.right_lane,
        nvgRGBA(right_lane_color, right_lane_color, right_lane_color, 128));

    // draw paths
    draw_path(s, scene->model.path.points, 0.0f, nvgRGBA(0xc0, 0xc0, 0xc0, 255));

    // draw MPC only if engaged
    if (scene->engaged) {
      draw_x_y(s, &scene->mpc_x[1], &scene->mpc_y[1], 19, nvgRGBA(255, 0, 0, 255));
    }
  }
}

static void ui_draw_vision(UIState *s) {
  const UIScene *scene = &s->scene;

  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  // hack for eon ui
  glEnable(GL_SCISSOR_TEST);
  glScissor(box_x, s->fb_h-(box_y+box_height), box_width, box_height);
  glViewport(box_x, s->fb_h-(box_y+box_height), box_width, box_height);
  draw_frame(s);
  glViewport(0, 0, s->fb_w, s->fb_h);
  glDisable(GL_SCISSOR_TEST);

  // nvg drawings
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  // glEnable(GL_CULL_FACE);

  glClear(GL_STENCIL_BUFFER_BIT);

  nvgBeginFrame(s->vg, s->fb_w, s->fb_h, 1.0f);

  nvgSave(s->vg);

  // hack for eon ui
  const int inner_height = box_width*9/16;
  nvgScissor(s->vg, box_x, box_y, box_width, box_height);
  nvgTranslate(s->vg, box_x, box_y + (box_height-inner_height)/2.0);
  nvgScale(s->vg, (float)box_width / s->fb_w, (float)inner_height / s->fb_h);

  if (!scene->frontview) {
    // ui_draw_transformed_box(s, 0xFF00FF00);
    ui_draw_world(s);

    if (scene->lead_status) {
      // 2.7 m fudge factor
      draw_cross(s, scene->lead_d_rel + 2.7, scene->lead_y_rel, 25,
                   nvgRGBA(255, 0, 0, 128));
    }

    const float label_size = 65.0f;

    nvgFontFace(s->vg, "courbd");

    if (scene->awareness_status > 0) {
      nvgBeginPath(s->vg);
      int bar_height = scene->awareness_status * 700;
      nvgRect(s->vg, 100, 300 + (700 - bar_height), 50, bar_height);
      nvgFillColor(s->vg, nvgRGBA(255 * (1 - scene->awareness_status),
                                  255 * scene->awareness_status, 0, 128));
      nvgFill(s->vg);
    }

    // Draw calibration progress (if needed)
    if (scene->cal_status == CALIBRATION_UNCALIBRATED) {
      int rec_width = 1120;
      int x_pos = 500;
      nvgBeginPath(s->vg);
      nvgStrokeWidth(s->vg, 14);
      nvgRoundedRect(s->vg, (1920-rec_width)/2, 920, rec_width, 150, 20);
      nvgStroke(s->vg);
      nvgFillColor(s->vg, nvgRGBA(0,0,0,180));
      nvgFill(s->vg);

      nvgFontSize(s->vg, label_size);
      nvgTextAlign(s->vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
      nvgFillColor(s->vg, nvgRGBA(255, 255, 255, 220));
      char calib_status_str[32];
      snprintf(calib_status_str, sizeof(calib_status_str), "Calibration In Progress: %d%%", scene->cal_perc);
      nvgText(s->vg, x_pos, 1010, calib_status_str, NULL);
    }
  }

  nvgRestore(s->vg);


  if (!ui_alert_active(s) && !scene->frontview) {
    // draw top bar

    const int bar_x = box_x;
    const int bar_y = box_y;
    const int bar_width = box_width;
    const int bar_height = 250 - box_y;

    assert(s->status < ARRAYSIZE(bg_colors));
    const uint8_t *color = bg_colors[s->status];

    nvgBeginPath(s->vg);
    nvgRect(s->vg, bar_x, bar_y, bar_width, bar_height);
    nvgFillColor(s->vg, nvgRGBA(color[0], color[1], color[2], color[3]));
    nvgFill(s->vg);

    const int message_y = box_y;
    const int message_height = bar_height;
    const int message_width = 800;
    const int message_x = box_x + box_width / 2 - message_width / 2;

    // message background
    nvgBeginPath(s->vg);
    NVGpaint bg = nvgLinearGradient(s->vg, message_x, message_y, message_x, message_y+message_height,
                                   nvgRGBAf(0.0, 0.0, 0.0, 0.0), nvgRGBAf(0.0, 0.0, 0.0, 0.1));
    nvgFillPaint(s->vg, bg);
    nvgRect(s->vg, message_x, message_y, message_width, message_height);
    nvgFill(s->vg);


    nvgFillColor(s->vg, nvgRGBA(255, 255, 255, 255));

    if (s->passive) {
      if (s->scene.started_ts > 0) {
        // draw drive time when passive

        uint64_t dt = nanos_since_boot() - s->scene.started_ts;

        nvgFontFace(s->vg, "sans-semibold");
        nvgFontSize(s->vg, 40*2.5);
        nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

        char time_str[64];
        if (dt > 60*60*1000000000ULL) {
          // hours
          snprintf(time_str, sizeof(time_str), "Drive time: %d:%02d:%02d",
            (int)(dt/(60*60*1000000000ULL)),
            (int)((dt%(60*60*1000000000ULL))/(60*1000000000ULL)),
            (int)(dt%(60*1000000000ULL)/1000000000ULL));
        } else {
          snprintf(time_str, sizeof(time_str), "Drive time: %d:%02d",
            (int)(dt/(60*1000000000ULL)),
            (int)(dt%(60*1000000000ULL)/1000000000ULL));
        }
        nvgText(s->vg, message_x+message_width/2, message_y+message_height/2+15, time_str, NULL);
      }
    } else {
      // status text
      nvgFontFace(s->vg, "sans-semibold");
      nvgFontSize(s->vg, 48*2.5);
      nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
      if (s->status == STATUS_DISENGAGED) {
        nvgText(s->vg, message_x+message_width/2, message_y+message_height/2+15, "DISENGAGED", NULL);
      } else if (s->status == STATUS_ENGAGED) {
        nvgText(s->vg, message_x+message_width/2, message_y+message_height/2+15, "ENGAGED", NULL);
      }
    }

    // set speed
    const int left_x = bar_x;
    const int left_y = bar_y;
    const int left_width = (bar_width - message_width) / 2;
    const int left_height = bar_height;

    nvgFontFace(s->vg, "sans-semibold");
    nvgFontSize(s->vg, 40*2.5);
    nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

    if (scene->v_cruise != 255 && scene->v_cruise != 0) {
      char speed_str[16];
      if (s->is_metric) {
        snprintf(speed_str, sizeof(speed_str), "%3d kph",
                 (int)(scene->v_cruise + 0.5));
      } else {
        /* Convert KPH to MPH. Using an approximated mph to kph
        conversion factor of 1.609 because this is what the Honda
        hud seems to be using */
        snprintf(speed_str, sizeof(speed_str), "%3d mph",
                 (int)(scene->v_cruise * 0.621504 + 0.5));
      }
      nvgText(s->vg, left_x+left_width/2, 115, speed_str, NULL);
    } else {
      nvgText(s->vg, left_x+left_width/2, 115, "N/A", NULL);
    }

    nvgFontFace(s->vg, "sans-regular");
    nvgFontSize(s->vg, 26*2.5);
    nvgText(s->vg, left_x+left_width/2, 185, "SET SPEED", NULL);

    // lead car
    const int right_y = bar_y;
    const int right_width = (bar_width - message_width) / 2;
    const int right_x = bar_x+bar_width-right_width;
    const int right_height = bar_height;

    nvgFontFace(s->vg, "sans-semibold");
    nvgFontSize(s->vg, 40*2.5);
    nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

    if (scene->lead_status) {
      char radar_str[16];
      // lead car is always in meters
      if (s->is_metric || true) {
        snprintf(radar_str, sizeof(radar_str), "%d m", (int)scene->lead_d_rel);
      } else {
        snprintf(radar_str, sizeof(radar_str), "%d ft", (int)(scene->lead_d_rel * 3.28084));
      }
      nvgText(s->vg, right_x+right_width/2, 115, radar_str, NULL);
    } else {
      nvgText(s->vg, right_x+right_width/2, 115, "N/A", NULL);
    }

    nvgFontFace(s->vg, "sans-regular");
    nvgFontSize(s->vg, 26*2.5);
    nvgText(s->vg, right_x+right_width/2, 185, "LEAD CAR", NULL);
  }

  nvgEndFrame(s->vg);

  glDisable(GL_BLEND);
  // glDisable(GL_CULL_FACE);
}

static void ui_draw_alerts(UIState *s) {
  const UIScene *scene = &s->scene;

  if (!ui_alert_active(s)) return;

  assert(s->status < ARRAYSIZE(alert_colors));
  const uint8_t *color = alert_colors[s->status];

  char alert_text1_upper[1024] = {0};
  for (int i=0; scene->alert_text1[i] && i < sizeof(alert_text1_upper)-1; i++) {
    alert_text1_upper[i] = toupper(scene->alert_text1[i]);
  }

  nvgBeginPath(s->vg);
  nvgRect(s->vg, box_x, box_y, box_width, box_height);
  nvgFillColor(s->vg, nvgRGBA(color[0], color[1], color[2], color[3]));
  nvgFill(s->vg);

  nvgFontFace(s->vg, "sans-semibold");

  if (strlen(alert_text1_upper) > 15) {
    nvgFontSize(s->vg, 72.0*2.5);
  } else {
    nvgFontSize(s->vg, 96.0*2.5);
  }
  nvgFillColor(s->vg, nvgRGBA(255, 255, 255, 255));
  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  nvgTextBox(s->vg, box_x + 50, box_y + 287, box_width - 50, alert_text1_upper, NULL);


  if (strlen(scene->alert_text2) > 0) {

    nvgFontFace(s->vg, "sans-regular");
    nvgFillColor(s->vg, nvgRGBA(255, 255, 255, 255));
    nvgFontSize(s->vg, 44.0*2.5);
    nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
    nvgTextBox(s->vg, box_x + 50, box_y + box_height - 250, box_width - 50, scene->alert_text2, NULL);
  }
}

static void ui_draw_blank(UIState *s) {
  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
}

static void ui_draw_aside(UIState *s) {
  char speed_str[32];

  nvgFillColor(s->vg, nvgRGBA(255, 255, 255, 255));
  nvgTextAlign(s->vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

  nvgFontFace(s->vg, "sans-semibold");
  nvgFontSize(s->vg, 110);
  if (s->is_metric) {
    snprintf(speed_str, sizeof(speed_str), "%d", (int)(s->scene.v_ego * 3.6 + 0.5));
  } else {
    snprintf(speed_str, sizeof(speed_str), "%d", (int)(s->scene.v_ego * 2.237 + 0.5));
  }
  nvgText(s->vg, 150, 762, speed_str, NULL);

  nvgFontFace(s->vg, "sans-regular");
  nvgFontSize(s->vg, 70);
  if (s->is_metric) {
    nvgText(s->vg, 150, 817, "kph", NULL);
  } else {
    nvgText(s->vg, 150, 817, "mph", NULL);
  }
}

static void ui_draw(UIState *s) {
  if (s->vision_connected && s->plus_state == 0) {
    ui_draw_vision(s);
  } else {
    ui_draw_blank(s);
  }

  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(s->vg, s->fb_w, s->fb_h, 1.0f);

    if (s->vision_connected) {
      ui_draw_aside(s);
    }
    ui_draw_alerts(s);

    nvgEndFrame(s->vg);
    glDisable(GL_BLEND);
  }

  eglSwapBuffers(s->display, s->surface);
  assert(glGetError() == GL_NO_ERROR);
}

static PathData read_path(cereal_ModelData_PathData_ptr pathp) {
  PathData ret = {0};

  struct cereal_ModelData_PathData pathd;
  cereal_read_ModelData_PathData(&pathd, pathp);

  ret.prob = pathd.prob;
  ret.std = pathd.std;

  capn_list32 pointl = pathd.points;
  capn_resolve(&pointl.p);
  for (int i = 0; i < 50; i++) {
    ret.points[i] = capn_to_f32(capn_get32(pointl, i));
  }

  return ret;
}

static ModelData read_model(cereal_ModelData_ptr modelp) {
  struct cereal_ModelData modeld;
  cereal_read_ModelData(&modeld, modelp);

  ModelData d = {0};

  d.path = read_path(modeld.path);
  d.left_lane = read_path(modeld.leftLane);
  d.right_lane = read_path(modeld.rightLane);

  struct cereal_ModelData_LeadData leadd;
  cereal_read_ModelData_LeadData(&leadd, modeld.lead);
  d.lead = (LeadData){
      .dist = leadd.dist, .prob = leadd.prob, .std = leadd.std,
  };

  return d;
}

static void update_status(UIState *s, int status) {
  if (s->status != status) {
    s->status = status;
    // wake up bg thread to change
    pthread_cond_signal(&s->bg_cond);
  }
}

static void ui_update(UIState *s) {
  int err;

  if (!s->intrinsic_matrix_loaded) {
    s->intrinsic_matrix_loaded = try_load_intrinsics(&s->intrinsic_matrix);
  }

  if (s->vision_connect_firstrun) {
    // cant run this in connector thread because opengl.
    // do this here for now in lieu of a run_on_main_thread event

    // setup frame texture
    glDeleteTextures(1, &s->frame_tex); //silently ignores a 0 texture
    glGenTextures(1, &s->frame_tex);
    glBindTexture(GL_TEXTURE_2D, s->frame_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, s->rgb_width, s->rgb_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    // BGR
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);

    // front
    glDeleteTextures(1, &s->frame_front_tex);
    glGenTextures(1, &s->frame_front_tex);
    glBindTexture(GL_TEXTURE_2D, s->frame_front_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, s->rgb_front_width, s->rgb_front_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    // BGR
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);

    assert(glGetError() == GL_NO_ERROR);

    s->vision_connect_firstrun = false;
  }

  // poll for events
  while (true) {
    zmq_pollitem_t polls[8] = {{0}};
    polls[0].socket = s->live100_sock_raw;
    polls[0].events = ZMQ_POLLIN;
    polls[1].socket = s->livecalibration_sock_raw;
    polls[1].events = ZMQ_POLLIN;
    polls[2].socket = s->model_sock_raw;
    polls[2].events = ZMQ_POLLIN;
    polls[3].socket = s->live20_sock_raw;
    polls[3].events = ZMQ_POLLIN;
    polls[4].socket = s->livempc_sock_raw;
    polls[4].events = ZMQ_POLLIN;
    polls[5].socket = s->thermal_sock_raw;
    polls[5].events = ZMQ_POLLIN;

    polls[6].socket = s->plus_sock_raw;
    polls[6].events = ZMQ_POLLIN;

    int num_polls = 7;
    if (s->vision_connected) {
      assert(s->ipc_fd >= 0);
      polls[7].fd = s->ipc_fd;
      polls[7].events = ZMQ_POLLIN;
      num_polls++;
    }

    int ret = zmq_poll(polls, num_polls, 0);
    if (ret < 0) {
      LOGW("poll failed (%d)", ret);
      break;
    }
    if (ret == 0) {
      break;
    }

    if (polls[0].revents || polls[1].revents || polls[2].revents ||
        polls[3].revents || polls[4].revents) {
      // awake on any (old) activity
      set_awake(s, true);
    }

    if (s->vision_connected && polls[7].revents) {
      // vision ipc event
      VisionPacket rp;
      err = vipc_recv(s->ipc_fd, &rp);
      if (err <= 0) {
        LOGW("vision disconnected");
        close(s->ipc_fd);
        s->ipc_fd = -1;
        s->vision_connected = false;
        continue;
      }
      if (rp.type == VIPC_STREAM_ACQUIRE) {
        bool front = rp.d.stream_acq.type == VISION_STREAM_UI_FRONT;
        int idx = rp.d.stream_acq.idx;

        int release_idx;
        if (front) {
          release_idx = s->cur_vision_front_idx;
        } else {
          release_idx = s->cur_vision_idx;
        }
        if (release_idx >= 0) {
          VisionPacket rep = {
            .type = VIPC_STREAM_RELEASE,
            .d = { .stream_rel = {
              .type = rp.d.stream_acq.type,
              .idx = release_idx,
            }},
          };
          vipc_send(s->ipc_fd, &rep);
        }

        if (front) {
          assert(idx < UI_BUF_COUNT);
          s->cur_vision_front_idx = idx;
          s->scene.bgr_front_ptr = s->front_bufs[idx].addr;
        } else {
          assert(idx < UI_BUF_COUNT);
          s->cur_vision_idx = idx;
          s->scene.bgr_ptr = s->bufs[idx].addr;
          // printf("v %d\n", ((uint8_t*)s->bufs[idx].addr)[0]);
        }
        if (front == s->scene.frontview) {
          ui_update_frame(s);
        }

      } else {
        assert(false);
      }
    } else if (polls[6].revents) {
      // plus socket

      zmq_msg_t msg;
      err = zmq_msg_init(&msg);
      assert(err == 0);
      err = zmq_msg_recv(&msg, s->plus_sock_raw, 0);
      assert(err >= 0);

      assert(zmq_msg_size(&msg) == 1);

      s->plus_state = ((char*)zmq_msg_data(&msg))[0];

      zmq_msg_close(&msg);

    } else {
      // zmq messages
      void* which = NULL;
      for (int i=0; i<6; i++) {
        if (polls[i].revents) {
          which = polls[i].socket;
          break;
        }
      }
      if (which == NULL) {
        continue;
      }

      zmq_msg_t msg;
      err = zmq_msg_init(&msg);
      assert(err == 0);
      err = zmq_msg_recv(&msg, which, 0);
      assert(err >= 0);

      struct capn ctx;
      capn_init_mem(&ctx, zmq_msg_data(&msg), zmq_msg_size(&msg), 0);

      cereal_Event_ptr eventp;
      eventp.p = capn_getp(capn_root(&ctx), 0, 1);
      struct cereal_Event eventd;
      cereal_read_Event(&eventd, eventp);

      if (eventd.which == cereal_Event_live100) {
        struct cereal_Live100Data datad;
        cereal_read_Live100Data(&datad, eventd.live100);

        s->scene.v_cruise = datad.vCruise;
        s->scene.v_ego = datad.vEgo;
        s->scene.curvature = datad.curvature;
        s->scene.engaged = datad.enabled;
        // printf("recv %f\n", datad.vEgo);

        s->scene.frontview = datad.rearViewCam;
        if (datad.alertText1.str) {
          snprintf(s->scene.alert_text1, sizeof(s->scene.alert_text1), "%s", datad.alertText1.str);
        } else {
          s->scene.alert_text1[0] = '\0';
        }
        if (datad.alertText2.str) {
          snprintf(s->scene.alert_text2, sizeof(s->scene.alert_text2), "%s", datad.alertText2.str);
        } else {
          s->scene.alert_text2[0] = '\0';
        }
        s->scene.awareness_status = datad.awarenessStatus;

        s->scene.alert_ts = eventd.logMonoTime;

        if (datad.alertStatus == cereal_Live100Data_AlertStatus_userPrompt) {
          update_status(s, STATUS_WARNING);
        } else if (datad.alertStatus == cereal_Live100Data_AlertStatus_critical) {
          update_status(s, STATUS_ALERT);
        } else if (datad.enabled) {
          update_status(s, STATUS_ENGAGED);
        } else {
          update_status(s, STATUS_DISENGAGED);
        }

      } else if (eventd.which == cereal_Event_live20) {
        struct cereal_Live20Data datad;
        cereal_read_Live20Data(&datad, eventd.live20);
        struct cereal_Live20Data_LeadData leaddatad;
        cereal_read_Live20Data_LeadData(&leaddatad, datad.leadOne);
        s->scene.lead_status = leaddatad.status;
        s->scene.lead_d_rel = leaddatad.dRel;
        s->scene.lead_y_rel = leaddatad.yRel;
        s->scene.lead_v_rel = leaddatad.vRel;
      } else if (eventd.which == cereal_Event_liveCalibration) {
        s->scene.world_objects_visible = s->intrinsic_matrix_loaded;
        struct cereal_LiveCalibrationData datad;
        cereal_read_LiveCalibrationData(&datad, eventd.liveCalibration);

        s->scene.cal_status = datad.calStatus;
        s->scene.cal_perc = datad.calPerc;

        // should we still even have this?
        capn_list32 warpl = datad.warpMatrix2;
        capn_resolve(&warpl.p);  // is this a bug?
        for (int i = 0; i < 3 * 3; i++) {
          s->scene.warp_matrix.v[i] = capn_to_f32(capn_get32(warpl, i));
        }

        capn_list32 extrinsicl = datad.extrinsicMatrix;
        capn_resolve(&extrinsicl.p);  // is this a bug?
        for (int i = 0; i < 3 * 4; i++) {
          s->scene.extrinsic_matrix.v[i] =
              capn_to_f32(capn_get32(extrinsicl, i));
        }
      } else if (eventd.which == cereal_Event_model) {
        s->scene.model_ts = eventd.logMonoTime;
        s->scene.model = read_model(eventd.model);
      } else if (eventd.which == cereal_Event_liveMpc) {
        struct cereal_LiveMpcData datad;
        cereal_read_LiveMpcData(&datad, eventd.liveMpc);

        capn_list32 x_list = datad.x;
        capn_resolve(&x_list.p);

        for (int i = 0; i < 50; i++){
          s->scene.mpc_x[i] = capn_to_f32(capn_get32(x_list, i));
        }

        capn_list32 y_list = datad.y;
        capn_resolve(&y_list.p);

        for (int i = 0; i < 50; i++){
          s->scene.mpc_y[i] = capn_to_f32(capn_get32(y_list, i));
        }
      } else if (eventd.which == cereal_Event_thermal) {
        struct cereal_ThermalData datad;
        cereal_read_ThermalData(&datad, eventd.thermal);

        if (!datad.started) {
          update_status(s, STATUS_STOPPED);
        } else if (s->status == STATUS_STOPPED) {
          // car is started but controls doesn't have fingerprint yet
          update_status(s, STATUS_DISENGAGED);
        }

        s->scene.started_ts = datad.startedTs;
      }

      capn_free(&ctx);

      zmq_msg_close(&msg);

    }
  }

}

static void* vision_connect_thread(void *args) {
  int err;

  UIState *s = args;
  while (!do_exit) {
    usleep(100000);
    pthread_mutex_lock(&s->lock);
    bool connected = s->vision_connected;
    pthread_mutex_unlock(&s->lock);
    if (connected) continue;

    int fd = vipc_connect();
    if (fd < 0) continue;



    VisionPacket p1 = {
      .type = VIPC_STREAM_SUBSCRIBE,
      .d = { .stream_sub = { .type = VISION_STREAM_UI_BACK, .tbuffer = true, }, },
    };
    err = vipc_send(fd, &p1);
    if (err < 0) {
      close(fd);
      continue;
    }
    VisionPacket p2 = {
      .type = VIPC_STREAM_SUBSCRIBE,
      .d = { .stream_sub = { .type = VISION_STREAM_UI_FRONT, .tbuffer = true, }, },
    };
    err = vipc_send(fd, &p2);
    if (err < 0) {
      close(fd);
      continue;
    }

    // printf("init recv\n");
    VisionPacket back_rp;
    err = vipc_recv(fd, &back_rp);
    if (err <= 0) {
      close(fd);
      continue;
    }
    assert(back_rp.type == VIPC_STREAM_BUFS);
    VisionPacket front_rp;
    err = vipc_recv(fd, &front_rp);
    if (err <= 0) {
      close(fd);
      continue;
    }
    assert(front_rp.type == VIPC_STREAM_BUFS);


    pthread_mutex_lock(&s->lock);
    assert(!s->vision_connected);
    s->ipc_fd = fd;

    ui_init_vision(s,
                   back_rp.d.stream_bufs, back_rp.num_fds, back_rp.fds,
                   front_rp.d.stream_bufs, front_rp.num_fds, front_rp.fds);

    s->vision_connected = true;
    s->vision_connect_firstrun = true;
    pthread_mutex_unlock(&s->lock);
  }
  return NULL;
}

#include <hardware/sensors.h>
#include <utils/Timers.h>

#define SENSOR_LIGHT 7

static void* light_sensor_thread(void *args) {
  int err;

  UIState *s = args;
  s->light_sensor = 0.0;

  struct sensors_poll_device_t* device;
  struct sensors_module_t* module;

  hw_get_module(SENSORS_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
  sensors_open(&module->common, &device);

  // need to do this
  struct sensor_t const* list;
  int count = module->get_sensors_list(module, &list);

  device->activate(device, SENSOR_LIGHT, 0);
  device->activate(device, SENSOR_LIGHT, 1);
  device->setDelay(device, SENSOR_LIGHT, ms2ns(100));

  while (!do_exit) {
    static const size_t numEvents = 1;
    sensors_event_t buffer[numEvents];

    int n = device->poll(device, buffer, numEvents);
    if (n < 0) {
      LOG_100("light_sensor_poll failed: %d", n);
    }
    if (n > 0) {
      s->light_sensor = buffer[0].light;
      //printf("new light sensor value: %f\n", s->light_sensor);
    }
  }

  return NULL;
}


static void* bg_thread(void* args) {
  UIState *s = args;

  EGLDisplay bg_display;
  EGLSurface bg_surface;

  FramebufferState *bg_fb = framebuffer_init("bg", 0x00001000, false,
                              &bg_display, &bg_surface, NULL, NULL);
  assert(bg_fb);

  bool first = true;
  while(!do_exit) {
    pthread_mutex_lock(&s->lock);

    if (first) {
      first = false;
    } else {
      pthread_cond_wait(&s->bg_cond, &s->lock);
    }

    assert(s->status < ARRAYSIZE(bg_colors));
    const uint8_t *color = bg_colors[s->status];

    pthread_mutex_unlock(&s->lock);

    glClearColor(color[0]/256.0, color[1]/256.0, color[2]/256.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);


    eglSwapBuffers(bg_display, bg_surface);
    assert(glGetError() == GL_NO_ERROR);

  }

  return NULL;
}


int main() {
  int err;

  zsys_handler_set(NULL);
  signal(SIGINT, (sighandler_t)set_do_exit);

  UIState uistate;
  UIState *s = &uistate;
  ui_init(s);

  pthread_t connect_thread_handle;
  err = pthread_create(&connect_thread_handle, NULL,
                       vision_connect_thread, s);
  assert(err == 0);

  pthread_t light_sensor_thread_handle;
  err = pthread_create(&light_sensor_thread_handle, NULL,
                       light_sensor_thread, s);
  assert(err == 0);

  pthread_t bg_thread_handle;
  err = pthread_create(&bg_thread_handle, NULL,
                       bg_thread, s);
  assert(err == 0);

  TouchState touch = {0};
  touch_init(&touch);

  // light sensor scaling params
  #define LIGHT_SENSOR_M 1.3
  #define LIGHT_SENSOR_B 5.0

  float smooth_light_sensor = LIGHT_SENSOR_B;

  while (!do_exit) {
    pthread_mutex_lock(&s->lock);

    float clipped_light_sensor = (s->light_sensor*LIGHT_SENSOR_M) + LIGHT_SENSOR_B;
    if (clipped_light_sensor > 255) clipped_light_sensor = 255;
    smooth_light_sensor = clipped_light_sensor * 0.01 + smooth_light_sensor * 0.99;
    set_brightness((int)smooth_light_sensor);

    ui_update(s);
    if (s->awake) {
      ui_draw(s);
    }

    // awake on any touch
    int touch_x = -1, touch_y = -1;
    int touched = touch_poll(&touch, &touch_x, &touch_y);
    if (touched == 1) {
      // touch event will still happen :(
      set_awake(s, true);
    }

    // manage wakefulness
    if (s->awake_timeout > 0) {
      s->awake_timeout--;
    } else {
      set_awake(s, false);
    }

    pthread_mutex_unlock(&s->lock);

    // no simple way to do 30fps vsync with surfaceflinger...
    usleep(30000);
  }

  set_awake(s, true);

  // wake up bg thread to exit
  pthread_mutex_lock(&s->lock);
  pthread_cond_signal(&s->bg_cond);
  pthread_mutex_unlock(&s->lock);
  err = pthread_join(bg_thread_handle, NULL);
  assert(err == 0);

  err = pthread_join(connect_thread_handle, NULL);
  assert(err == 0);

  return 0;
}
