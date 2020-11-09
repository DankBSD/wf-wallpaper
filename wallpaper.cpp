#define WAYFIRE_PLUGIN
#define WLR_USE_UNSTABLE
#include <wayfire/compositor-view.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
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

struct shader_t : public OpenGL::program_t {
	~shader_t() { free_resources(); }
};

using renderable_t = std::variant<wf::simple_texture_t, shader_t>;

static std::shared_ptr<renderable_t> load_renderable(shm_header *hdr, size_t len) {
	const auto *bytes = reinterpret_cast<unsigned char *>(hdr);
	std::shared_ptr<renderable_t> rdr = nullptr;
	OpenGL::render_begin();
	if (hdr->fmt == loaded_fmt::texture_r8g8b8 || hdr->fmt == loaded_fmt::texture_r8g8b8a8) {
		// TODO check rowstride*height does not exceed len
		rdr = std::make_shared<renderable_t>(std::in_place_type<wf::simple_texture_t>);
		auto &tex = std::get<wf::simple_texture_t>(*rdr);
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
		prg.set_simple(OpenGL::compile_program(vertex_shader, fs_header + fs_body + fs_footer));
	}
	OpenGL::render_end();
	return rdr;
}

static void render_renderable(renderable_t &rbl, const sizing mode, const wf::framebuffer_t &fb,
                              const float blend) {
	std::visit(
	    [&](auto &&arg) {
		    using T = std::decay_t<decltype(arg)>;
		    if constexpr (std::is_same_v<T, wf::simple_texture_t>) {
			    OpenGL::render_texture(wf::texture_t{arg.tex}, fb, apply_mode(mode, fb.geometry, arg),
			                           glm::vec4(1, 1, 1, blend), OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
		    } else if constexpr (std::is_same_v<T, shader_t>) {
			    arg.use(wf::TEXTURE_TYPE_RGBA);
			    static const float vertexData[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
			    arg.attrib_pointer("position", 2, 0, vertexData);
			    arg.uniform1f("_wfwp_blend_", blend);
			    arg.uniform3f("iResolution", fb.geometry.width, fb.geometry.height, 1.0);
			    // TODO: more shadertoy variables
			    GL_CALL(glEnable(GL_BLEND));
			    GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
			    GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
			    arg.deactivate();
		    }
	    },
	    rbl);
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
		LOGD("Unloading wallpaper ", path);
		wf::get_core().get_data<loadable_cache_t>()->storage.erase(path);
	}
};

struct wallpaper_view_t : public wf::color_rect_view_t {
	std::shared_ptr<loadable_t> from, to;
	wf::signal_connection_t do_damage{[this](wf::signal_data_t *) { damage(); }};

	void set_color(wf::color_t col) {
		_color = col;  // inherited field
		damage();
	}

	void set_from(std::shared_ptr<loadable_t> from_) {
		if (from && !from->renderable) from->disconnect_signal(&do_damage);
		from = from_;
		if (from && !from->renderable) from->connect_signal("loaded", &do_damage);
		damage();
	}

	void set_to(std::shared_ptr<loadable_t> to_) {
		if (to && !to->renderable) to->disconnect_signal(&do_damage);
		to = to_;
		if (to && !to->renderable) to->connect_signal("loaded", &do_damage);
		damage();
	}

	void simple_render(const wf::framebuffer_t &fb, int x, int y,
	                   const wf::region_t &damage) override {
		auto mode = sizing::fill;

		OpenGL::render_begin(fb);
		for (auto &box : damage) {
			LOGD("Damage ", box.x1, ",", box.y1, " - ", box.x2, ",", box.y2);
			fb.logic_scissor(wlr_box_from_pixman_box(box));
			// TODO: show color for transparent images too
			if (!(from && from->renderable) && !(to && to->renderable)) {
				wf::color_t premultiply{_color.r * _color.a, _color.g * _color.a, _color.b * _color.a,
				                        _color.a};
				OpenGL::render_rectangle({x, y, fb.geometry.width, fb.geometry.width}, premultiply,
				                         fb.get_orthographic_projection());
			}
			if (from && from->renderable) /*(wps->fade_animation.running() && from && from->tex !=
			                                 (uint32_t) -1)*/
				render_renderable(*from->renderable, mode, fb, 1.0);
			if (to && to->renderable) render_renderable(*to->renderable, mode, fb, 1.0);
		}
		OpenGL::render_end();
	}
};

struct wallpaper_config : public noncopyable_t {
	inline static const std::string wildcard = "*";
	wf::option_wrapper_t<std::string> path, outputs, workspaces;
	wf::option_wrapper_t<wf::color_t> color;
	std::vector<nonstd::observer_ptr<wallpaper_view_t>> views;
	std::shared_ptr<loadable_t> loadable;

	wallpaper_config(std::string &secname) {
		outputs.load_option(secname + "/outputs");
		workspaces.load_option(secname + "/workspaces");

		path.load_option(secname + "/path");
		loadable = wf::get_core().get_data<loadable_cache_t>()->load_file(path);
		path.set_callback([this]() {
			loadable = wf::get_core().get_data<loadable_cache_t>()->load_file(path);
			for (auto view : views) view->set_to(loadable);
		});

		color.load_option(secname + "/color");
		color.set_callback([this]() {
			for (auto view : views) view->set_color(color);
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

struct wayfire_wallpaper : public wf::plugin_interface_t {
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
		auto grid = output->workspace->get_workspace_grid_size();
		for (int x = 0; x < grid.width; x++) {
			for (int y = 0; y < grid.height; y++) {
				ws_views[x + grid.width * y]->set_geometry(
				    {x * og.width, y * og.height, og.width, og.height});
				ws_views[x + grid.width * y]->damage();
			}
		}
	}};

	wf::signal_connection_t pre_remove{[this](wf::signal_data_t *data) { fini(); }};

	void init() override {
		wf::get_core().store_data<loadable_cache_t>(std::make_unique<loadable_cache_t>());
		grab_interface->name = "wallpaper";
		grab_interface->capabilities = 0;

		auto og = output->get_relative_geometry();
		auto grid = output->workspace->get_workspace_grid_size();
		ws_views.resize(grid.width * grid.height);
		for (int x = 0; x < grid.width; x++) {
			for (int y = 0; y < grid.height; y++) {
				auto view = std::make_unique<wallpaper_view_t>();
				view->set_output(output);
				view->set_geometry({x * og.width, y * og.height, og.width, og.height});
				view->role = wf::VIEW_ROLE_DESKTOP_ENVIRONMENT;
				output->workspace->add_view(view, wf::LAYER_BACKGROUND);
				ws_views[x + grid.width * y] = {view};
				wf::get_core().add_view(std::move(view));
			}
		}

		output->connect_signal("output-configuration-changed", &output_configuration_changed);
		output->connect_signal("workspace-changed", &workspace_changed);
		output->connect_signal("pre-remove", &pre_remove);
		wf::get_core().connect_signal("reload-config", &reload_config);
		load_config();
	}

	void fini() override {
		// NOTE: used on pre-remove
		for (auto view : ws_views) {
			view->from.reset();
			view->to.reset();
			view->close();
		}
		ws_views.clear();
		output->render->damage_whole();
		confs.clear();
		// NOTE: fini != destructor -- MUST erase everything that uses cache (references loadables) here
		wf::get_core().erase_data<loadable_cache_t>();
	}
};

DECLARE_WAYFIRE_PLUGIN(wayfire_wallpaper);
