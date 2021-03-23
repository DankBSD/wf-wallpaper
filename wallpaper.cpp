#define WAYFIRE_PLUGIN
#define WLR_USE_UNSTABLE
#include <linux/input-event-codes.h>

#include <wayfire/compositor-view.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/singleton-plugin.hpp>
#include <wayfire/util/duration.hpp>
// #include <wayfire/output-layout.hpp>
// #include <wayfire/workspace-stream.hpp>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cassert>
#include <locale>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <wayfire/plugins/common/simple-texture.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/workspace-manager.hpp>

#include "clocks.hpp"
#include "pid_fd_fork.h"

enum class sizing { fill, fit, stretch };

std::ostream &operator<<(std::ostream &out, sizing f) {
	if (f == sizing::fill) return out << "fill";
	if (f == sizing::fit) return out << "fit";
	if (f == sizing::stretch) return out << "stretch";
	return out;
};

static wf::geometry_t apply_mode(const sizing mode, const wf::geometry_t &fbgeom,
                                 const wf::simple_texture_t &tex) {
	if (tex.width == fbgeom.width && tex.height == fbgeom.height) return fbgeom;
	auto result = fbgeom;
	// LOGD("Applying scaling mode ", mode);
	// LOGD(" Framebuffer ", fbgeom.width, "x", fbgeom.height, " @ ", fbgeom.x, ",", fbgeom.y,
	//      " - ratio ", fbgeom.width / (double)fbgeom.height);
	// LOGD(" Texture ", tex.width, "x", tex.height, " - ratio ", tex.width / (double)tex.height);
	double width_ratio = tex.width / (double)fbgeom.width;
	double height_ratio = tex.height / (double)fbgeom.height;
	if (mode == sizing::fill) {
		if (width_ratio > height_ratio) {
			auto old_width = result.width;
			result.width *= (width_ratio / height_ratio);
			result.x += (old_width - result.width) / 2;
		} else {
			auto old_height = result.height;
			result.height *= (height_ratio / width_ratio);
			result.y += (old_height - result.height) / 2;
		}
	} else if (mode == sizing::fit) {
		if (tex.width >= tex.height) {  // XXX: width_ratio > height_ratio too or not?
			auto old_height = result.height;
			result.height *= (height_ratio / width_ratio);
			result.y += (old_height - result.height) / 2;
		} else {
			auto old_width = result.width;
			result.width *= (width_ratio / height_ratio);
			result.x += (old_width - result.width) / 2;
		}
	}
	// LOGD(" New framebuffer geom ", result.width, "x", result.height, " - ratio ",
	//      result.width / (double)result.height);
	return result;
}

enum class loaded_fmt : size_t {
	glsl = 0x420420,
	texture_r8g8b8 = 0x69696969,
	texture_r8g8b8a8,
};

struct shm_header {
	loaded_fmt fmt;
	size_t width, height, rowstride, len;
};

static pid_fork_t start_file_loader(std::string path, int *shm_fd) {
	pid_fork_t pf;
	int shm = memfd_create("wf-wallpaper", 0);
	if (shm <= 0) {
		LOGE("Could not create shared memory, WTF?");
		return pf;
	}
	int pid = pid_fork_start(&pf);
	if (pid < 0) {
		LOGE("Could not fork, WTF?");
		return pf;
	}
	if (pid != 0) {
		*shm_fd = shm;
		return pf;
	}
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	assert(fd != -1);
	gchar snippet[1025] = {0};
	ssize_t readlen = read(fd, snippet, 1024);
	assert(readlen > 0);
	close(fd);
	// All valid UTF8 in the first KiB == must be GLSL rather than binary
	if (g_utf8_validate(snippet, readlen, NULL)) {
		gsize file_len = 0;
		gchar *file_cont = nullptr;
		assert(g_file_get_contents(path.c_str(), &file_cont, &file_len, NULL));
		size_t shm_len = sizeof(shm_header) + file_len;
		assert(ftruncate(shm, shm_len) == 0);
		auto *bytes = reinterpret_cast<uint8_t *>(
		    mmap(nullptr, shm_len, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0));
		assert(bytes != MAP_FAILED);
		auto *hdr = reinterpret_cast<shm_header *>(bytes);
		hdr->fmt = loaded_fmt::glsl;
		memcpy(bytes + sizeof(shm_header), file_cont, file_len);
		g_free(file_cont);
		assert(munmap(bytes, shm_len) == 0);
		_Exit(0);
	}
	g_autoptr(GdkPixbuf) pixb = gdk_pixbuf_new_from_file(path.c_str(), NULL);
	assert(pixb != nullptr);
	size_t shm_len = sizeof(shm_header) + gdk_pixbuf_get_byte_length(pixb);
	assert(ftruncate(shm, shm_len) == 0);
	auto *bytes = reinterpret_cast<uint8_t *>(
	    mmap(nullptr, shm_len, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0));
	assert(bytes != MAP_FAILED);
	auto *hdr = reinterpret_cast<shm_header *>(bytes);
	hdr->fmt =
	    gdk_pixbuf_get_has_alpha(pixb) ? loaded_fmt::texture_r8g8b8a8 : loaded_fmt::texture_r8g8b8;
	hdr->width = gdk_pixbuf_get_width(pixb);
	hdr->height = gdk_pixbuf_get_height(pixb);
	hdr->rowstride = gdk_pixbuf_get_rowstride(pixb);
	hdr->len = gdk_pixbuf_get_byte_length(pixb);
	memcpy(bytes + sizeof(shm_header), gdk_pixbuf_read_pixels(pixb), hdr->len);
	assert(munmap(bytes, shm_len) == 0);
	_Exit(0); /* do not run any inherited atexit handlers (GPU drivers in particular crash) */
}

struct picture_t : public wf::simple_texture_t {
	bool has_alpha;
};

struct shader_t : public OpenGL::program_t {
	~shader_t() { free_resources(); }
	bool uses_time, uses_time_delta, uses_date, uses_frame, uses_mouse;
};

using renderable_t = std::variant<picture_t, shader_t>;

static std::shared_ptr<renderable_t> load_renderable(shm_header *hdr, size_t len) {
	const auto *bytes = reinterpret_cast<unsigned char *>(hdr);
	std::shared_ptr<renderable_t> rdr = nullptr;
	OpenGL::render_begin();
	if (hdr->fmt == loaded_fmt::texture_r8g8b8 || hdr->fmt == loaded_fmt::texture_r8g8b8a8) {
		// TODO check rowstride*height does not exceed len
		rdr = std::make_shared<renderable_t>(std::in_place_type<picture_t>);
		auto &tex = std::get<picture_t>(*rdr);
		tex.width = hdr->width;
		tex.height = hdr->height;
		if (tex.tex == (GLuint)-1) GL_CALL(glGenTextures(1, &tex.tex));
		GL_CALL(glBindTexture(GL_TEXTURE_2D, tex.tex));
		GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		auto channels = (hdr->fmt == loaded_fmt::texture_r8g8b8) ? 3 : 4;
		if (hdr->rowstride % channels != 0)
			LOGE("Row stride ", hdr->rowstride, " not a multiple of ", channels);
		GL_CALL(glPixelStorei(GL_UNPACK_ROW_LENGTH, hdr->rowstride / channels));
		auto storage_fmt = (hdr->fmt == loaded_fmt::texture_r8g8b8) ? GL_RGB8 : GL_RGBA8;
		GL_CALL(glTexStorage2D(GL_TEXTURE_2D, 1, storage_fmt, hdr->width, hdr->height));
		auto tex_fmt = (hdr->fmt == loaded_fmt::texture_r8g8b8) ? GL_RGB : GL_RGBA;
		tex.has_alpha = tex_fmt == GL_RGBA;
		GL_CALL(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hdr->width, hdr->height, tex_fmt,
		                        GL_UNSIGNED_BYTE, (GLvoid *)(bytes + sizeof(shm_header))));
		GL_CALL(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
		GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
	} else if (hdr->fmt == loaded_fmt::glsl) {
		static const char *vertex_shader = R"(
#version 300 es
precision mediump float;
uniform vec3 iResolution;
in vec2 position;
out vec2 fragCoord;
void main() {
	gl_Position = vec4(position.xy, 0.0, 1.0);
	fragCoord = (gl_Position.xy + vec2(1.0)) / vec2(2.0) * iResolution.xy;
}
)";
		static const std::string fs_header = R"(
#version 300 es
precision mediump float;
uniform float _wfwp_blend_;
uniform vec3      iResolution;
uniform float     iTime;
uniform float     iTimeDelta;
uniform int       iFrame;
uniform float     iChannelTime[4];
uniform vec3      iChannelResolution[4];
uniform vec4      iMouse;
uniform sampler2D iChannel0;
uniform sampler2D iChannel1;
uniform sampler2D iChannel2;
uniform sampler2D iChannel3;
uniform vec4      iDate;
uniform float     iSampleRate;
in vec2 fragCoord;
out vec4 _wfwp_out_color_;
)";
		static const std::string fs_footer = R"(
void main() {
	vec4 shResult;
	mainImage(shResult, fragCoord);
	_wfwp_out_color_ = vec4(shResult.xyz, _wfwp_blend_);
}
)";
		std::string fs_body((const char *)(bytes + sizeof(shm_header)), len - sizeof(shm_header));
		rdr = std::make_shared<renderable_t>(std::in_place_type<shader_t>);
		auto &prg = std::get<shader_t>(*rdr);
		prg.uses_time = fs_body.find("iTime") != std::string::npos;
		prg.uses_time_delta = fs_body.find("iTimeDelta") != std::string::npos;
		prg.uses_date = fs_body.find("iDate") != std::string::npos;
		prg.uses_frame = fs_body.find("iFrame") != std::string::npos;
		prg.uses_mouse = fs_body.find("iMouse") != std::string::npos;
		prg.set_simple(OpenGL::compile_program(vertex_shader, fs_header + fs_body + fs_footer));
	}
	OpenGL::render_end();
	return rdr;
}

struct loadable_t;

struct loadable_cache_t : public wf::custom_data_t {
	std::unordered_map<std::string, std::weak_ptr<loadable_t>> storage;

	std::shared_ptr<loadable_t> load_file(const std::string &path) {
		std::string res_path(PATH_MAX + 1, '\0');
		if (realpath(path.c_str(), res_path.data()) == NULL) {
			LOGE("Could not resolve path ", path, ": ", errno, " - ", strerror(errno));
			return nullptr;
		}
		if (storage.count(res_path) > 0) {
			return storage[res_path].lock();
		}
		auto result = std::make_shared<loadable_t>(res_path);
		storage.emplace(res_path, result);
		return result;
	}
};

struct loadable_t : public noncopyable_t, public wf::signal_provider_t {
	std::shared_ptr<renderable_t> renderable;
	std::string path;
	pid_fork_t loader_proc = {-1, -1};
	wl_event_source *fdsrc = nullptr;
	int shm_fd = -1;

	static int loader_done(int, uint32_t, void *data) {
#define FAILFAIL                                       \
	do {                                                 \
		LOGE("Wallpaper loading failed for ", self->path); \
		close(self->shm_fd);                               \
		return -1;                                         \
	} while (false)
		auto *self = reinterpret_cast<loadable_t *>(data);
		wl_event_source_remove(self->fdsrc);
		self->fdsrc = nullptr;
		int code = -1;
		if (pid_fork_exit_code(&self->loader_proc, &code) != 0 || code != 0) FAILFAIL;
		struct stat sb;
		if (fstat(self->shm_fd, &sb) == -1) FAILFAIL;
		LOGD("Wallpaper shm size ", sb.st_size);
		auto *bytes = reinterpret_cast<uint8_t *>(
		    mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, self->shm_fd, 0));
		if (bytes == MAP_FAILED) FAILFAIL;
		self->renderable = load_renderable(reinterpret_cast<shm_header *>(bytes), sb.st_size);
		if (munmap(bytes, sb.st_size) != 0) FAILFAIL;
		close(self->shm_fd);
		self->shm_fd = -1;
		LOGI("Wallpaper loaded for ", self->path);
		self->emit_signal("loaded", nullptr);
		return 0;
#undef FAILFAIL
	}

	loadable_t(std::string path_) : path(path_) {
		LOGI("Wallpaper starting loading for ", path);
		loader_proc = start_file_loader(path.c_str(), &shm_fd);
		fdsrc = wl_event_loop_add_fd(wf::get_core().ev_loop, loader_proc.poll_fd, WL_EVENT_READABLE,
		                             loader_done, this);
	}

	~loadable_t() {
		if (fdsrc) {
			LOGD("Canceling loading of wallpaper ", path);
			wl_event_source_remove(fdsrc);
			pid_fork_signal(&loader_proc, SIGTERM);
			int code = -1;
			pid_fork_exit_code(&loader_proc, &code);
			LOGD("Canceled loading of wallpaper ", path, " - exit code ", code);
		} else {
			LOGD("Unloading wallpaper ", path);
		}
		wf::get_core()
		    .get_data_safe<wf::detail::singleton_data_t<loadable_cache_t>>()
		    ->ptr.storage.erase(path);
	}
};

struct wallpaper_view_t : public wf::color_rect_view_t {
	wf::signal_connection_t something_loaded{[this](wf::signal_data_t *) { on_something_loaded(); }};
	std::shared_ptr<loadable_t> from, to;
	fast_mono_clock::time_point from_start, to_start, current_frame_time, last_frame_time;
	glm::vec4 current_frame_date;
	std::chrono::duration<float /* seconds */> last_frame_delta;
	int mouse_x = 0, mouse_y = 0, mouse_pre_x = 0, mouse_pre_y = 0, mouse_click_x = 0,
	    mouse_click_y = 0;
	int64_t from_frames = 0, to_frames = 0, total_frames = 0;
	bool animate = false;
	bool mouse = false;
	int frameskip = 1;
	sizing mode = sizing::fill;

	void tick_animation() {
		if (from && from->renderable) {
			from_frames++;
		}
		if (to && to->renderable) {
			to_frames++;
		}
		if ((++total_frames % frameskip) == 0) {
			if (check<shader_t>([](auto &arg) { return arg.uses_time; })) {
				current_frame_time = fast_mono_clock::now();
			}
			if (check<shader_t>([](auto &arg) { return arg.uses_time_delta; })) {
				last_frame_delta = current_frame_time - last_frame_time;
				last_frame_time = current_frame_time;
			}
			if (check<shader_t>([](auto &arg) { return arg.uses_date; })) {
				struct tm date;
				time_t date_tt = fast_wall_clock::to_time_t(fast_wall_clock::now());
				localtime_r(&date_tt, &date);
				current_frame_date = {date.tm_year + 1900, date.tm_mon /*0-11!*/, date.tm_mday,
				                      date.tm_sec + date.tm_min * 60 + date.tm_hour * 3600};
				// XXX: original shadertoy also added milliseconds, but time_t has rounded them
			}
			damage();
		}
	}

	void on_something_loaded() {
		damage();
		animate = check<shader_t>([](auto &arg) {
			return arg.uses_time || arg.uses_time_delta || arg.uses_date || arg.uses_frame;
		});
		mouse = check<shader_t>([](auto &arg) { return arg.uses_mouse; });
	}

	void set_color(wf::color_t col) override {
		_color = col;  // inherited field
		damage();
	}

	void set_sizing_mode(std::string m) {
		if (m == "fit") {
			mode = sizing::fit;
		} else if (m == "stretch") {
			mode = sizing::stretch;
		} else {
			mode = sizing::fill;
		}
		damage();
	}

	void set_from(std::shared_ptr<loadable_t> from_) {
		if (from && !from->renderable) from->disconnect_signal(&something_loaded);
		from = from_;
		from_start = fast_mono_clock::now();
		from_frames = 0;
		if (from && !from->renderable) from->connect_signal("loaded", &something_loaded);
		on_something_loaded();
	}

	void set_to(std::shared_ptr<loadable_t> to_) {
		if (to && !to->renderable) to->disconnect_signal(&something_loaded);
		to = to_;
		to_start = fast_mono_clock::now();
		to_frames = 0;
		if (to && !to->renderable) to->connect_signal("loaded", &something_loaded);
		on_something_loaded();
	}

	void render_renderable(renderable_t &rbl, const wf::framebuffer_t &fb, const float blend,
	                       fast_mono_clock::time_point &start_time, int64_t frames) {
		std::visit(
		    [&](auto &&arg) {
			    using T = std::decay_t<decltype(arg)>;
			    if constexpr (std::is_same_v<T, picture_t>) {
				    OpenGL::render_texture(wf::texture_t{arg.tex}, fb, apply_mode(mode, fb.geometry, arg),
				                           glm::vec4(1, 1, 1, blend), OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
			    } else if constexpr (std::is_same_v<T, shader_t>) {
				    // NOTE: we mutate the actual renderable here, but that's fine,
				    //       just have to make sure we don't leave any state to get mixed between
				    //       invocations
				    arg.use(wf::TEXTURE_TYPE_RGBA);
				    static const float vertexData[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
				    arg.attrib_pointer("position", 2, 0, vertexData);
				    arg.uniform1f("_wfwp_blend_", blend);
				    arg.uniform3f("iResolution", fb.geometry.width, fb.geometry.height, 1.0);
				    arg.uniform4f("iMouse", {mouse_x, mouse_y, mouse_click_x, mouse_click_y});
				    if (arg.uses_time) {
					    std::chrono::duration<float /* seconds */> dur = current_frame_time - start_time;
					    arg.uniform1f("iTime", dur.count());
				    }
				    if (arg.uses_time_delta) {
					    arg.uniform1f("iTimeDelta", last_frame_delta.count());
				    }
				    arg.uniform1f("iFrame", frames);
				    arg.uniform4f("iDate", current_frame_date);
				    GL_CALL(glEnable(GL_BLEND));
				    GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
				    GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
				    arg.deactivate();
			    }
		    },
		    rbl);
	}

	template <typename R, typename F>
	inline bool check_renderable(renderable_t &rbl, F cb) {
		return std::visit(
		    [&](auto &&arg) {
			    using T = std::decay_t<decltype(arg)>;
			    if constexpr (std::is_same_v<T, R>) {
				    return cb(arg);
			    }
			    return false;
		    },
		    rbl);
	}

	template <typename R, typename F>
	inline bool check(F cb) {
		return (to && to->renderable && check_renderable<R>(*to->renderable, cb)) ||
		       (from && from->renderable && check_renderable<R>(*from->renderable, cb));
	}

	void simple_render(const wf::framebuffer_t &fb, int x, int y,
	                   const wf::region_t &damage) override {
		OpenGL::render_begin(fb);
		for (auto &box : damage) {
			LOGD("Damage ", box.x1, ",", box.y1, " - ", box.x2, ",", box.y2, " == ", box.x2 - box.x1, "x",
			     box.y2 - box.y1);
			fb.logic_scissor(wlr_box_from_pixman_box(box));
			bool no_renderables = !(from && from->renderable) && !(to && to->renderable);
			bool transparent_pics = check<picture_t>([](auto &arg) { return arg.has_alpha; });
			if (no_renderables || transparent_pics || mode == sizing::fit) {
				wf::color_t premultiply{_color.r * _color.a, _color.g * _color.a, _color.b * _color.a,
				                        _color.a};
				OpenGL::render_rectangle({x, y, fb.geometry.width, fb.geometry.width}, premultiply,
				                         fb.get_orthographic_projection());
			}
			if (from && from->renderable) /*(wps->fade_animation.running() && from && from->tex !=
			                                 (uint32_t) -1)*/
				render_renderable(*from->renderable, fb, 1.0, from_start, from_frames);
			if (to && to->renderable) render_renderable(*to->renderable, fb, 1.0, to_start, to_frames);
		}
		OpenGL::render_end();
	}

	bool accepts_input(int32_t sx, int32_t sy) override {
		return mouse && 0 <= sx && sx < geometry.width && 0 <= sy && sy < geometry.height;
	}

	void on_pointer_motion(int x, int y) override {
		if (mouse_click_x == 0 && mouse_click_y == 0) {
			mouse_pre_x = x;
			mouse_pre_y = y;
		} else {
			mouse_x = x;
			mouse_y = y;
		}
		if (mouse_click_x != 0 && mouse_click_y != 0) {
			damage();
		}
	}
	void on_pointer_button(uint32_t button, uint32_t state) override {
		if (button != BTN_LEFT) {
			return;
		}
		if (state == WLR_BUTTON_PRESSED) {
			mouse_click_x = mouse_pre_x;
			mouse_click_y = mouse_pre_y;
		} else {
			mouse_click_x = 0;
			mouse_click_y = 0;
			damage();
		}
	}

	void on_touch_down(int x, int y) override {
		mouse_click_x = x;
		mouse_click_y = y;
	}
	void on_touch_up() override {
		mouse_click_x = 0;
		mouse_click_y = 0;
		damage();
	}
	void on_touch_motion(int x, int y) override {
		mouse_x = x;
		mouse_y = y;
	}
};

struct wallpaper_config : public noncopyable_t {
	inline static const std::string wildcard = "*";
	wf::option_wrapper_t<std::string> path, outputs, workspaces, sizing_mode;
	wf::option_wrapper_t<wf::color_t> color;
	wf::option_wrapper_t<int> frameskip;
	std::vector<nonstd::observer_ptr<wallpaper_view_t>> views;
	std::shared_ptr<loadable_t> loadable;

	wallpaper_config(std::string &secname) {
		outputs.load_option(secname + "/outputs");
		workspaces.load_option(secname + "/workspaces");
		sizing_mode.load_option(secname + "/sizing_mode");

		path.load_option(secname + "/path");
		loadable = wf::get_core()
		               .get_data_safe<wf::detail::singleton_data_t<loadable_cache_t>>()
		               ->ptr.load_file(path);
		path.set_callback([this]() {
			loadable = wf::get_core()
			               .get_data_safe<wf::detail::singleton_data_t<loadable_cache_t>>()
			               ->ptr.load_file(path);
			for (auto view : views) view->set_to(loadable);
		});

		for (auto view : views) view->set_sizing_mode(sizing_mode);
		sizing_mode.set_callback([this]() {
			for (auto view : views) view->set_sizing_mode(sizing_mode);
		});

		color.load_option(secname + "/color");
		for (auto view : views) view->set_color(color);
		color.set_callback([this]() {
			for (auto view : views) view->set_color(color);
		});

		frameskip.load_option(secname + "/frameskip");
		for (auto view : views) view->frameskip = frameskip;
		frameskip.set_callback([this]() {
			for (auto view : views) view->frameskip = frameskip;
		});
	}

	uint32_t workspace_specificity() const { return (std::string)workspaces == wildcard ? 0 : 1; }

	void reset_views() { views.clear(); }

	bool match_add_view(int idx, nonstd::observer_ptr<wallpaper_view_t> ptr) {
		bool matches = (std::string)workspaces == wildcard;
		if (!matches) {
			std::stringstream ss(workspaces);
			for (int i; ss >> i;) {
				if (i == idx) {
					matches = true;
					break;
				}
				if (ss.peek() == ',' || std::isspace((char)ss.peek(), std::locale::classic())) ss.ignore();
			}
		}
		if (matches) {
			views.push_back(ptr);
			ptr->set_to(loadable);
			ptr->set_color(color);
			ptr->set_sizing_mode(sizing_mode);
			ptr->frameskip = frameskip;
		}
		LOGI((std::string)path, " matching ", idx, " - ", matches);
		return matches;
	}
};

struct cmp_specificity {
	bool operator()(const nonstd::observer_ptr<wallpaper_config> &a,
	                const nonstd::observer_ptr<wallpaper_config> &b) const {
		return a->workspace_specificity() > b->workspace_specificity();
	}
};

struct wayfire_wallpaper : public wf::singleton_plugin_t<loadable_cache_t> {
	// wf::animation::simple_animation_t fade_animation{100};

	std::vector<nonstd::observer_ptr<wallpaper_view_t>> ws_views;
	std::map<std::string, wallpaper_config> confs;
	std::vector<nonstd::observer_ptr<wallpaper_config>> confs_order;

	void load_config() {
		confs_order.clear();
		auto &config = wf::get_core().config;
		for (auto &sec : config.get_all_sections()) {
			auto secname = sec->get_name();
			size_t splitter = secname.find_first_of(":");
			if (splitter == std::string::npos) {
				continue;
			}
			auto obj_type_name = secname.substr(0, splitter);
			if (obj_type_name != "wallpaper") {
				continue;
			}
			// auto key = secname.substr(splitter + 1);
			confs.try_emplace(secname, secname);
		}
		for (auto it = confs.begin(); it != confs.end();) {  // note how this pass does 3 things
			if (config.get_section(it->first) == nullptr) {
				it = confs.erase(it);
			} else {
				nonstd::observer_ptr<wallpaper_config> p{&it->second};
				auto oit = std::lower_bound(confs_order.begin(), confs_order.end(), p, cmp_specificity{});
				confs_order.insert(oit, p);
				it->second.reset_views();
				it++;
			}
		}
		int idx = 0;
		for (auto view : ws_views) {
			for (auto conf : confs_order) {
				if (conf->match_add_view(idx, view)) {
					break;
				}
			}
			idx++;
		}
	}

	wf::signal_connection_t reload_config{[this](wf::signal_data_t *sigdata) { load_config(); }};

	wf::signal_connection_t workspace_changed{[this](wf::signal_data_t *sigdata) {
		// Do what workspace impl's set_workspace does for fixed_views
		// (would be better to have a fixed flag on the view)
		auto *data = static_cast<wf::workspace_changed_signal *>(sigdata);
		auto screen = output->get_screen_size();
		auto dx = (data->old_viewport.x - data->new_viewport.x) * screen.width;
		auto dy = (data->old_viewport.y - data->new_viewport.y) * screen.height;
		auto grid = output->workspace->get_workspace_grid_size();
		for (auto view : ws_views) {
			auto vg = view->get_wm_geometry();
			view->move(vg.x + dx, vg.y + dy);
		}
	}};

	wf::signal_connection_t output_configuration_changed{[this](wf::signal_data_t *sigdata) {
		wf::output_configuration_changed_signal *data =
		    static_cast<wf::output_configuration_changed_signal *>(sigdata);

		if (!data->changed_fields || (data->changed_fields & wf::OUTPUT_SOURCE_CHANGE) ||
		    ws_views.empty()) {
			return;
		}

		auto og = output->get_relative_geometry();
		auto ws = output->workspace->get_current_workspace();
		auto grid = output->workspace->get_workspace_grid_size();
		for (int x = 0; x < grid.width; x++) {
			for (int y = 0; y < grid.height; y++) {
				ws_views[x + grid.width * y]->set_geometry(
				    {(x - ws.x) * og.width, (y - ws.y) * og.height, og.width, og.height});
				ws_views[x + grid.width * y]->damage();
			}
		}
	}};

	wf::signal_connection_t workspace_grid_changed{[this](wf::signal_data_t *sigdata) {
		wf::workspace_grid_changed_signal *data =
		    static_cast<wf::workspace_grid_changed_signal *>(sigdata);
		for (auto &conf : confs) {
			conf.second.reset_views();
		}
		auto old_count = data->old_grid_size.width * data->old_grid_size.height;
		auto new_count = data->new_grid_size.width * data->new_grid_size.height;
		for (int i = new_count; i < old_count; i++) {
			ws_views[i]->from.reset();
			ws_views[i]->to.reset();
			ws_views[i]->close();
		}
		ws_views.resize(new_count);
		for (int i = old_count; i < new_count; i++) {
			ws_views[i] = new_view();
		}
		auto og = output->get_relative_geometry();
		auto ws = output->workspace->get_current_workspace();
		for (int x = 0; x < data->new_grid_size.width; x++) {
			for (int y = 0; y < data->new_grid_size.height; y++) {
				ws_views[x + data->new_grid_size.width * y]->set_geometry(
				    {(x - ws.x) * og.width, (y - ws.y) * og.height, og.width, og.height});
				ws_views[x + data->new_grid_size.width * y]->damage();
			}
		}
		load_config();
	}};

	wf::effect_hook_t tick_animations = [=]() {
		bool sr = false;
		auto ws = output->workspace->get_current_workspace();
		for (auto view : ws_views) {
			if (view->animate) {
				view->tick_animation();
				if (view->frameskip != 1 && output->workspace->view_visible_on(view, ws)) {
					sr = true;
				}
			}
		}
		if (sr) {
			// We need this to make frameskip work.
			// Otherwise, skip with nothing else on the workspace -> no damage on a frame
			// -> effect hook does't fire next frame because there's no frame at all
			output->render->schedule_redraw();
		}
	};

	wf::signal_connection_t pre_remove{[this](wf::signal_data_t *data) { clear(); }};

	nonstd::observer_ptr<wallpaper_view_t> new_view() {
		auto view = std::make_unique<wallpaper_view_t>();
		nonstd::observer_ptr<wallpaper_view_t> obs{view};
		view->set_output(output);
		view->role = wf::VIEW_ROLE_DESKTOP_ENVIRONMENT;
		output->workspace->add_view(view, wf::LAYER_BACKGROUND);
		wf::get_core().add_view(std::move(view));
		return obs;
	}

	void init() override {
		singleton_plugin_t::init();

		// add_view trips assertion on noop. not like we need to waste resources on noop anyway
		if (wlr_output_is_noop(output->handle)) return;

		grab_interface->name = "wallpaper";
		grab_interface->capabilities = 0;

		auto og = output->get_relative_geometry();
		auto grid = output->workspace->get_workspace_grid_size();
		ws_views.resize(grid.width * grid.height);
		for (int x = 0; x < grid.width; x++) {
			for (int y = 0; y < grid.height; y++) {
				ws_views[x + grid.width * y] = new_view();
				ws_views[x + grid.width * y]->set_geometry(
				    {x * og.width, y * og.height, og.width, og.height});
			}
		}

		output->connect_signal("workspace-grid-changed", &workspace_grid_changed);
		output->connect_signal("output-configuration-changed", &output_configuration_changed);
		output->connect_signal("workspace-changed", &workspace_changed);
		output->connect_signal("pre-remove", &pre_remove);
		output->render->add_effect(&tick_animations, wf::OUTPUT_EFFECT_PRE);
		wf::get_core().connect_signal("reload-config", &reload_config);
		load_config();
	}

	void clear() {
		// NOTE: used on pre-remove
		output->render->rem_effect(&tick_animations);
		for (auto view : ws_views) {
			view->from.reset();
			view->to.reset();
			view->close();
		}
		ws_views.clear();
		output->render->damage_whole();
		confs.clear();
	}

	void fini() override {
		if (!wlr_output_is_noop(output->handle)) {
			clear();
		}
		// NOTE: fini != destructor -- MUST erase everything that uses cache (references loadables) here
		singleton_plugin_t::fini();
	}
};

DECLARE_WAYFIRE_PLUGIN(wayfire_wallpaper);
