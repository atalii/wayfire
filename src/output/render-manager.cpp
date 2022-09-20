#include "wayfire/render-manager.hpp"
#include "view/view-impl.hpp"
#include "wayfire/core.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/region.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/view.hpp"
#include "wayfire/workspace-stream.hpp"
#include "wayfire/output.hpp"
#include "../core/core-impl.hpp"
#include "wayfire/util.hpp"
#include "wayfire/workspace-manager.hpp"
#include "../core/seat/seat.hpp"
#include "../core/opengl-priv.hpp"
#include "../main.hpp"
#include <algorithm>
#include <wayfire/nonstd/reverse.hpp>
#include <wayfire/nonstd/safe-list.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

namespace wf
{
/**
 * output_damage_t is responsible for tracking the damage on a given output.
 */
struct output_damage_t
{
    signal::connection_t<scene::root_node_update_signal> root_update;
    std::vector<scene::render_instance_uptr> render_instances;

    wf::wl_listener_wrapper on_damage_destroy;

    wf::region_t frame_damage;
    wlr_output *output;
    wlr_output_damage *damage_manager;
    output_t *wo;

    void update_scenegraph()
    {
        auto root = wf::get_core().scene();

        scene::damage_callback push_damage = [=] (wf::region_t region)
        {
            // Damage is pushed up to the root in root coordinate system,
            // we need it in layout-local coordinate system.
            region += -wf::origin(wo->get_layout_geometry());
            this->damage(region);
        };

        render_instances.clear();
        root->gen_render_instances(render_instances, push_damage);
    }

    output_damage_t(output_t *output)
    {
        this->output = output->handle;
        this->wo     = output;

        damage_manager = wlr_output_damage_create(this->output);

        on_damage_destroy.set_callback([=] (void*) { damage_manager = nullptr; });
        on_damage_destroy.connect(&damage_manager->events.destroy);

        auto root = wf::get_core().scene();
        root_update = [=] (scene::root_node_update_signal *data)
        {
            if (!(data->flags & scene::update_flag::CHILDREN_LIST) &&
                !(data->flags & scene::update_flag::ENABLED))
            {
                return;
            }

            update_scenegraph();
        };

        root->connect<scene::root_node_update_signal>(&root_update);
        update_scenegraph();
    }

    /**
     * Damage the given region
     */
    void damage(const wf::region_t& region)
    {
        if (region.empty() || !damage_manager)
        {
            return;
        }

        /* Wlroots expects damage after scaling */
        auto scaled_region = region * wo->handle->scale;
        frame_damage |= scaled_region;
        wlr_output_damage_add(damage_manager, scaled_region.to_pixman());
    }

    void damage(const wf::geometry_t& box)
    {
        if ((box.width <= 0) || (box.height <= 0) || !damage_manager)
        {
            return;
        }

        /* Wlroots expects damage after scaling */
        auto scaled_box = box * wo->handle->scale;
        frame_damage |= scaled_box;
        wlr_output_damage_add_box(damage_manager, &scaled_box);
    }

    wf::region_t acc_damage;

    /**
     * Make the output current. This sets its EGL context as current, checks
     * whether there is any damage and makes sure frame_damage contains all the
     * damage needed for repainting the next frame.
     */
    bool make_current(bool& needs_swap)
    {
        if (!damage_manager)
        {
            return false;
        }

        auto r = wlr_output_damage_attach_render(damage_manager, &needs_swap,
            acc_damage.to_pixman());

        if (!r)
        {
            return false;
        }

        needs_swap |= force_next_frame;
        force_next_frame = false;

        return true;
    }

    /**
     * Accumulate damage from last frame.
     * Needs to be called after make_current()
     */
    void accumulate_damage()
    {
        frame_damage |= acc_damage;
        if (runtime_config.no_damage_track)
        {
            frame_damage |= get_wlr_damage_box();
        }
    }

    /**
     * Return the damage that has been scheduled for the next frame up to now,
     * or, if in a repaint, the damage for the current frame
     */
    wf::region_t get_scheduled_damage()
    {
        if (!damage_manager)
        {
            return {};
        }

        return frame_damage * (1.0 / wo->handle->scale);
    }

    /**
     * Swap the output buffers. Also clears the scheduled damage.
     */
    void swap_buffers(wf::region_t& swap_damage)
    {
        if (!output)
        {
            return;
        }

        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);

        /* Make sure that the damage is in buffer coordinates */
        wl_output_transform transform =
            wlr_output_transform_invert(output->transform);
        wlr_region_transform(swap_damage.to_pixman(), swap_damage.to_pixman(),
            transform, w, h);

        wlr_output_set_damage(output,
            const_cast<wf::region_t&>(swap_damage).to_pixman());
        wlr_output_commit(output);
        frame_damage.clear();
    }

    bool force_next_frame = false;
    /**
     * Schedule a frame for the output
     */
    void schedule_repaint()
    {
        wlr_output_schedule_frame(output);
        force_next_frame = true;
    }

    /**
     * Return the extents of the visible region for the output in the wlroots
     * damage coordinate system.
     */
    wlr_box get_wlr_damage_box() const
    {
        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);

        return {0, 0, w, h};
    }

    /**
     * Same as render_manager::get_ws_box()
     */
    wlr_box get_ws_box(wf::point_t ws) const
    {
        auto current = wo->workspace->get_current_workspace();

        wlr_box box = wo->get_relative_geometry();
        box.x = (ws.x - current.x) * box.width;
        box.y = (ws.y - current.y) * box.height;

        return box;
    }

    /**
     * Returns the scheduled damage for the given workspace, in output-local
     * coordinates.
     */
    wf::region_t get_ws_damage(wf::point_t ws)
    {
        auto scaled = frame_damage * (1.0 / wo->handle->scale);

        return scaled & get_ws_box(ws);
    }

    /**
     * Same as render_manager::damage_whole()
     */
    void damage_whole()
    {
        auto vsize = wo->workspace->get_workspace_grid_size();
        auto vp    = wo->workspace->get_current_workspace();
        auto res   = wo->get_screen_size();

        damage(wf::geometry_t{
                -vp.x * res.width,
                -vp.y * res.height,
                vsize.width * res.width,
                vsize.height * res.height,
            });
    }

    wf::wl_idle_call idle_damage;
    /**
     * Same as render_manager::damage_whole_idle()
     */
    void damage_whole_idle()
    {
        damage_whole();
        if (!idle_damage.is_connected())
        {
            idle_damage.run_once([&] () { damage_whole(); });
        }
    }
};

/**
 * Very simple class to manage effect hooks
 */
struct effect_hook_manager_t
{
    using effect_container_t = wf::safe_list_t<effect_hook_t*>;
    effect_container_t effects[OUTPUT_EFFECT_TOTAL];

    void add_effect(effect_hook_t *hook, output_effect_type_t type)
    {
        effects[type].push_back(hook);
    }

    bool can_scanout() const
    {
        return effects[OUTPUT_EFFECT_OVERLAY].size() == 0 &&
               effects[OUTPUT_EFFECT_POST].size() == 0;
    }

    void rem_effect(effect_hook_t *hook)
    {
        for (int i = 0; i < OUTPUT_EFFECT_TOTAL; i++)
        {
            effects[i].remove_all(hook);
        }
    }

    void run_effects(output_effect_type_t type)
    {
        effects[type].for_each([] (auto effect)
        { (*effect)(); });
    }
};

/**
 * A class to manage and run postprocessing effects
 */
struct postprocessing_manager_t
{
    using post_container_t = wf::safe_list_t<post_hook_t*>;
    post_container_t post_effects;
    wf::framebuffer_t post_buffers[3];
    /* Buffer to which other operations render to */
    static constexpr uint32_t default_out_buffer = 0;

    output_t *output;
    uint32_t output_width, output_height;
    postprocessing_manager_t(output_t *output)
    {
        this->output = output;
    }

    void workaround_wlroots_backend_y_invert(wf::render_target_t& fb) const
    {
        /* Sometimes, the framebuffer by OpenGL is Y-inverted.
         * This is the case only if the target framebuffer is not 0 */
        if (output_fb == 0)
        {
            return;
        }

        fb.wl_transform = wlr_output_transform_compose(
            (wl_output_transform)fb.wl_transform, WL_OUTPUT_TRANSFORM_FLIPPED_180);
        fb.transform = get_output_matrix_from_transform(
            (wl_output_transform)fb.wl_transform);
    }

    uint32_t output_fb = 0;
    void set_output_framebuffer(uint32_t output_fb)
    {
        this->output_fb = output_fb;
    }

    void allocate(int width, int height)
    {
        if (post_effects.size() == 0)
        {
            return;
        }

        output_width  = width;
        output_height = height;

        OpenGL::render_begin();
        post_buffers[default_out_buffer].allocate(width, height);
        OpenGL::render_end();
    }

    void add_post(post_hook_t *hook)
    {
        post_effects.push_back(hook);
        output->render->damage_whole_idle();
    }

    void rem_post(post_hook_t *hook)
    {
        post_effects.remove_all(hook);
        output->render->damage_whole_idle();
    }

    /* Run all postprocessing effects, rendering to alternating buffers and
     * finally to the screen.
     *
     * NB: 2 buffers just aren't enough. We render to the zero buffer, and then
     * we alternately render to the second and the third. The reason: We track
     * damage. So, we need to keep the whole buffer each frame. */
    void run_post_effects()
    {
        wf::framebuffer_t default_framebuffer;
        default_framebuffer.fb  = output_fb;
        default_framebuffer.tex = 0;

        int last_buffer_idx = default_out_buffer;
        int next_buffer_idx = 1;

        post_effects.for_each([&] (auto post) -> void
        {
            /* The last postprocessing hook renders directly to the screen, others to
             * the currently free buffer */
            wf::framebuffer_t& next_buffer =
                (post == post_effects.back() ? default_framebuffer :
                    post_buffers[next_buffer_idx]);

            OpenGL::render_begin();
            /* Make sure we have the correct resolution */
            next_buffer.allocate(output_width, output_height);
            OpenGL::render_end();

            (*post)(post_buffers[last_buffer_idx], next_buffer);

            last_buffer_idx  = next_buffer_idx;
            next_buffer_idx ^= 0b11; // alternate 1 and 2
        });
    }

    wf::render_target_t get_target_framebuffer() const
    {
        wf::render_target_t fb;
        fb.geometry     = output->get_relative_geometry();
        fb.wl_transform = output->handle->transform;
        fb.transform    = get_output_matrix_from_transform(
            (wl_output_transform)fb.wl_transform);
        fb.scale = output->handle->scale;

        if (post_effects.size())
        {
            fb.fb  = post_buffers[default_out_buffer].fb;
            fb.tex = post_buffers[default_out_buffer].tex;
        } else
        {
            fb.fb  = output_fb;
            fb.tex = 0;
        }

        workaround_wlroots_backend_y_invert(fb);
        fb.viewport_width  = output->handle->width;
        fb.viewport_height = output->handle->height;

        return fb;
    }

    bool can_scanout() const
    {
        return post_effects.size() == 0;
    }
};

/**
 * Responsible for attaching depth buffers to framebuffers.
 * It keeps at most 3 depth buffers at any given time to conserve
 * resources.
 */
class depth_buffer_manager_t
{
  public:
    void ensure_depth_buffer(int fb, int width, int height)
    {
        /* If the backend doesn't have its own framebuffer, then the
         * framebuffer is created with a depth buffer. */
        if (fb == 0)
        {
            return;
        }

        attach_buffer(find_buffer(fb), fb, width, height);
    }

    depth_buffer_manager_t() = default;

    ~depth_buffer_manager_t()
    {
        OpenGL::render_begin();
        for (auto& buffer : buffers)
        {
            GL_CALL(glDeleteTextures(1, &buffer.tex));
        }

        OpenGL::render_end();
    }

    depth_buffer_manager_t(const depth_buffer_manager_t &) = delete;
    depth_buffer_manager_t(depth_buffer_manager_t &&) = delete;
    depth_buffer_manager_t& operator =(const depth_buffer_manager_t&) = delete;
    depth_buffer_manager_t& operator =(depth_buffer_manager_t&&) = delete;

  private:
    static constexpr size_t MAX_BUFFERS = 3;

    struct depth_buffer_t
    {
        GLuint tex = -1;
        int attached_to = -1;
        int width  = 0;
        int height = 0;

        int64_t last_used = 0;
    };

    void attach_buffer(depth_buffer_t& buffer, int fb, int width, int height)
    {
        if ((buffer.attached_to == fb) &&
            (buffer.width == width) &&
            (buffer.height == height))
        {
            return;
        }

        if (buffer.tex != (GLuint) - 1)
        {
            GL_CALL(glDeleteTextures(1, &buffer.tex));
        }

        GL_CALL(glGenTextures(1, &buffer.tex));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, buffer.tex));
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
            width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL));
        buffer.width  = width;
        buffer.height = height;

        GL_CALL(glBindTexture(GL_TEXTURE_2D, buffer.tex));
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, buffer.tex, 0));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

        buffer.attached_to = fb;
        buffer.last_used   = get_current_time();
    }

    depth_buffer_t& find_buffer(int fb)
    {
        for (auto& buffer : buffers)
        {
            if (buffer.attached_to == fb)
            {
                return buffer;
            }
        }

        /** New buffer? */
        if (buffers.size() < MAX_BUFFERS)
        {
            buffers.push_back(depth_buffer_t{});

            return buffers.back();
        }

        /** Evict oldest */
        auto oldest = &buffers.front();
        for (auto& buffer : buffers)
        {
            if (buffer.last_used < oldest->last_used)
            {
                oldest = &buffer;
            }
        }

        return *oldest;
    }

    std::vector<depth_buffer_t> buffers;
};

/**
 * A struct which manages the repaint delay.
 *
 * The repaint delay is a technique to potentially lower the input latency.
 *
 * It works by delaying Wayfire's repainting after getting the next frame event.
 * During this time the clients have time to update and submit their buffers.
 * If they manage this on time, the next frame will contain the already new
 * application contents, otherwise, the changes are visible after 1 more frame.
 *
 * The repaint delay however should be chosen so that Wayfire's own rendering
 * starts early enough for the next vblank, otherwise, the framerate will suffer.
 *
 * Calculating the maximal time Wayfire needs for rendering is very hard, and
 * and can change depending on active plugins, number of opened windows, etc.
 *
 * Thus, we need to dynamically guess this time based on the previous frames.
 * Currently, the following algorithm is implemented:
 *
 * Initially, the repaint delay is zero.
 *
 * If at some point Wayfire skips a frame, the delay is assumed too big and
 * reduced by `2^i`, where `i` is the amount of consecutive skipped frames.
 *
 * If Wayfire renders in time for `increase_window` milliseconds, then the
 * delay is increased by one. If the next frame is delayed, then
 * `increase_window` is doubled, otherwise, it is halved
 * (but it must stay between `MIN_INCREASE_WINDOW` and `MAX_INCREASE_WINDOW`).
 */
struct repaint_delay_manager_t
{
    repaint_delay_manager_t(wf::output_t *output)
    {
        on_present.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_output_event_present*>(data);
            this->refresh_nsec = ev->refresh;
        });
        on_present.connect(&output->handle->events.present);
    }

    /**
     * The next frame will be skipped.
     */
    void skip_frame()
    {
        // Mark last frame as invalid, because we don't know how much time
        // will pass until next frame
        last_pageflip = -1;
    }

    /**
     * Starting a new frame.
     */
    void start_frame()
    {
        if (last_pageflip == -1)
        {
            last_pageflip = get_current_time();
            return;
        }

        const int64_t refresh = this->refresh_nsec / 1e6;
        const int64_t on_time_thresh = refresh * 1.5;
        const int64_t last_frame_len = get_current_time() - last_pageflip;
        if (last_frame_len <= on_time_thresh)
        {
            // We rendered last frame on time
            if (get_current_time() - last_increase >= increase_window)
            {
                increase_window = clamp(int64_t(increase_window * 0.75),
                    MIN_INCREASE_WINDOW, MAX_INCREASE_WINDOW);
                update_delay(+1);
                reset_increase_timer();

                // If we manage the next few frames, then we have reached a new
                // stable state
                expand_inc_window_on_miss = 20;
            } else
            {
                --expand_inc_window_on_miss;
            }

            // Stop exponential decrease
            consecutive_decrease = 1;
        } else
        {
            // We missed last frame.
            update_delay(-consecutive_decrease);
            // Next decrease should be faster
            consecutive_decrease = clamp(consecutive_decrease * 2, 1, 32);

            // Next increase should be tried after a longer interval
            if (expand_inc_window_on_miss >= 0)
            {
                increase_window = clamp(increase_window * 2,
                    MIN_INCREASE_WINDOW, MAX_INCREASE_WINDOW);
            }

            reset_increase_timer();
        }

        last_pageflip = get_current_time();
    }

    /**
     * @return The delay in milliseconds for the current frame.
     */
    int get_delay()
    {
        return delay;
    }

  private:
    int delay = 0;

    void update_delay(int delta)
    {
        int config_delay = std::max(0,
            (int)(this->refresh_nsec / 1e6) - max_render_time);

        int min = 0;
        int max = config_delay;
        if (max_render_time == -1)
        {
            max = 0;
        } else if (!dynamic_delay)
        {
            min = config_delay;
            max = config_delay;
        }

        delay = clamp(delay + delta, min, max);
    }

    void reset_increase_timer()
    {
        last_increase = get_current_time();
    }

    static constexpr int64_t MIN_INCREASE_WINDOW = 200; // 200 ms
    static constexpr int64_t MAX_INCREASE_WINDOW = 30'000; // 30s
    int64_t increase_window = MIN_INCREASE_WINDOW;
    int64_t last_increase   = 0;

    // > 0 => Increase increase_window
    int64_t expand_inc_window_on_miss = 0;

    // Expontential decrease in case of missed frames
    int32_t consecutive_decrease = 1;

    // Time of last frame
    int64_t last_pageflip = -1; // -1 is invalid

    int64_t refresh_nsec;
    wf::option_wrapper_t<int> max_render_time{"core/max_render_time"};
    wf::option_wrapper_t<bool> dynamic_delay{"workarounds/dynamic_repaint_delay"};

    wf::wl_listener_wrapper on_present;
};

class wf::render_manager::impl
{
  public:
    wf::wl_listener_wrapper on_frame;
    wf::wl_timer repaint_timer;

    output_t *output;
    wf::region_t swap_damage;
    std::unique_ptr<output_damage_t> output_damage;
    std::unique_ptr<effect_hook_manager_t> effects;
    std::unique_ptr<postprocessing_manager_t> postprocessing;
    std::unique_ptr<depth_buffer_manager_t> depth_buffer_manager;
    std::unique_ptr<repaint_delay_manager_t> delay_manager;

    wf::option_wrapper_t<wf::color_t> background_color_opt;

    impl(output_t *o) :
        output(o)
    {
        output_damage = std::make_unique<output_damage_t>(o);
        effects = std::make_unique<effect_hook_manager_t>();
        postprocessing = std::make_unique<postprocessing_manager_t>(o);
        depth_buffer_manager = std::make_unique<depth_buffer_manager_t>();
        delay_manager = std::make_unique<repaint_delay_manager_t>(o);

        on_frame.set_callback([&] (void*)
        {
            delay_manager->start_frame();

            auto repaint_delay = delay_manager->get_delay();
            // Leave a bit of time for clients to render, see
            // https://github.com/swaywm/sway/pull/4588
            if (repaint_delay < 1)
            {
                paint();
            } else
            {
                output->handle->frame_pending = true;
                repaint_timer.set_timeout(repaint_delay, [=] ()
                {
                    output->handle->frame_pending = false;
                    paint();
                    return false;
                });
            }

            send_frame_done();
        });
        on_frame.connect(&output_damage->damage_manager->events.frame);

        default_stream.scale_x    = default_stream.scale_y = 1;
        default_stream.buffer.tex = 0;

        background_color_opt.load_option("core/background_color");
        background_color_opt.set_callback([=] ()
        {
            output_damage->damage_whole_idle();
        });

        output_damage->schedule_repaint();
    }

    // Workspace stream for the current workspace, drawn on the output's buffer
    workspace_stream_t default_stream;

    render_hook_t renderer;
    void set_renderer(render_hook_t rh)
    {
        renderer = rh;
        output_damage->damage_whole_idle();
    }

    int constant_redraw_counter = 0;
    void set_redraw_always(bool always)
    {
        constant_redraw_counter += (always ? 1 : -1);
        if (constant_redraw_counter > 1) /* no change, exit */
        {
            return;
        }

        if (constant_redraw_counter < 0)
        {
            LOGE("constant_redraw_counter got below 0!");
            constant_redraw_counter = 0;

            return;
        }

        output_damage->schedule_repaint();
    }

    int output_inhibit_counter = 0;
    void add_inhibit(bool add)
    {
        output_inhibit_counter += add ? 1 : -1;
        if (output_inhibit_counter == 0)
        {
            output_damage->damage_whole_idle();

            wf::output_start_rendering_signal data;
            data.output = output;
            output->emit_signal("start-rendering", &data);
        }
    }

    /* Actual rendering functions */

    /**
     * Bind the output's EGL surface, allocate buffers
     */
    void bind_output(uint32_t fb)
    {
        OpenGL::bind_output(output, fb);

        /* Make sure the default buffer has enough size */
        postprocessing->allocate(output->handle->width, output->handle->height);
    }

    /**
     * The default renderer, which just makes sure the correct workspace stream
     * is drawn to the framebuffer
     */
    void default_renderer()
    {
        if (runtime_config.damage_debug)
        {
            /* Clear the screen to yellow, so that the repainted parts are
             * visible */
            swap_damage |= output_damage->get_wlr_damage_box();

            OpenGL::render_begin(output->handle->width, output->handle->height,
                postprocessing->output_fb);
            OpenGL::clear({1, 1, 0, 1});
            OpenGL::render_end();
        }

        default_stream.ws = output->workspace->get_current_workspace();
        workspace_stream_update(default_stream);
    }

    wayfire_view get_first_view_recursive(wf::scene::node_ptr node)
    {
        if (auto vnode = dynamic_cast<wf::scene::view_node_t*>(node.get()))
        {
            return vnode->get_view();
        }

        for (auto& ch : node->get_children())
        {
            auto view = get_first_view_recursive(ch);
            if (view)
            {
                return view;
            }
        }

        return nullptr;
    }

    wayfire_view find_scanout_view()
    {
        for (int layer = (int)wf::scene::layer::ALL_LAYERS - 1; layer >= 0; layer--)
        {
            if (auto view = get_first_view_recursive(
                output->node_for_layer((wf::scene::layer)layer)))
            {
                return view;
            }
        }

        return nullptr;
    }

    wayfire_view last_scanout;
    /**
     * Try to directly scanout a view
     */
    bool do_direct_scanout()
    {
        const bool can_scanout =
            !wf::get_core_impl().seat->drag_active &&
            !output_inhibit_counter &&
            !renderer &&
            effects->can_scanout() &&
            postprocessing->can_scanout();

        if (!can_scanout)
        {
            return false;
        }

        auto candidate = find_scanout_view();
        if (!candidate)
        {
            return false;
        }

        // The candidate must cover the whole output
        if (candidate->get_output_geometry() != output->get_relative_geometry())
        {
            return false;
        }

        // The view must have only a single surface and no transformers
        if (candidate->has_transformer() ||
            !candidate->priv->surface_children_above.empty() ||
            !candidate->children.empty())
        {
            return false;
        }

        // Must have a wlr surface with the correct scale and transform
        auto surface = candidate->get_wlr_surface();
        if (!surface ||
            (surface->current.scale != output->handle->scale) ||
            (surface->current.transform != output->handle->transform))
        {
            return false;
        }

        // Finally, the opaque region must be the full surface.
        wf::region_t non_opaque = output->get_relative_geometry();
        non_opaque ^= candidate->get_opaque_region(wf::point_t{0, 0});
        if (!non_opaque.empty())
        {
            return false;
        }

        wlr_presentation_surface_sampled_on_output(
            wf::get_core().protocols.presentation, surface, output->handle);
        wlr_output_attach_buffer(output->handle, &surface->buffer->base);

        if (wlr_output_commit(output->handle))
        {
            if (candidate != last_scanout)
            {
                last_scanout = candidate;
                LOGD("Scanned out ",
                    candidate->get_title(), ",", candidate->get_app_id());
            }

            return true;
        } else
        {
            LOGD("Failed to scan out view ", candidate->get_title());
            return false;
        }
    }

    /**
     * Return the swap damage if called from overlay or postprocessing
     * effect callbacks or empty region otherwise.
     */
    wf::region_t get_swap_damage()
    {
        return swap_damage;
    }

    /**
     * Render an output. Either calls the built-in renderer, or the render hook
     * of a plugin
     */
    void render_output()
    {
        if (renderer)
        {
            renderer(postprocessing->get_target_framebuffer());
            /* TODO: let custom renderers specify what they want to repaint... */
            swap_damage |= output_damage->get_wlr_damage_box();
        } else
        {
            swap_damage =
                output_damage->get_scheduled_damage() * output->handle->scale;
            swap_damage &= output_damage->get_wlr_damage_box();
            default_renderer();
        }
    }

    void update_bound_output()
    {
        int current_fb;
        GL_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fb));
        bind_output(current_fb);

        postprocessing->set_output_framebuffer(current_fb);
        const auto& default_fb = postprocessing->get_target_framebuffer();
        depth_buffer_manager->ensure_depth_buffer(
            default_fb.fb, default_fb.viewport_width, default_fb.viewport_height);

        default_stream.buffer.fb = current_fb;
    }

    /**
     * Repaints the whole output, includes all effects and hooks
     */
    void paint()
    {
        /* Part 1: frame setup: query damage, etc. */
        effects->run_effects(OUTPUT_EFFECT_PRE);
        effects->run_effects(OUTPUT_EFFECT_DAMAGE);

        if (do_direct_scanout())
        {
            // Yet another optimization: if we can directly scanout, we should
            // stop the rest of the repaint cycle.
            return;
        } else
        {
            last_scanout = nullptr;
        }

        bool needs_swap;
        if (!output_damage->make_current(needs_swap))
        {
            wlr_output_rollback(output->handle);
            delay_manager->skip_frame();
            return;
        }

        if (!needs_swap && !constant_redraw_counter)
        {
            /* Optimization: the output doesn't need a swap (so isn't damaged),
             * and no plugin wants custom redrawing - we can just skip the whole
             * repaint */
            wlr_output_rollback(output->handle);
            delay_manager->skip_frame();
            return;
        }

        // Accumulate damage now, when we are sure we will render the frame.
        // Doing this earlier may mean that the damage from the previous frames
        // creeps into the current frame damage, if we had skipped a frame.
        output_damage->accumulate_damage();

        update_bound_output();

        /* Part 2: call the renderer, which sets swap_damage and
         * draws the scenegraph */
        render_output();

        /* Part 3: overlay effects */
        effects->run_effects(OUTPUT_EFFECT_OVERLAY);

        if (postprocessing->post_effects.size())
        {
            swap_damage |= output_damage->get_wlr_damage_box();
        }

        /* Part 4: finalize the scene: postprocessing effects */
        postprocessing->run_post_effects();
        if (output_inhibit_counter)
        {
            OpenGL::render_begin(output->handle->width, output->handle->height,
                postprocessing->output_fb);
            OpenGL::clear({0, 0, 0, 1});
            OpenGL::render_end();
        }

        /* Part 5: render sw cursors
         * We render software cursors after everything else
         * for consistency with hardware cursor planes */
        OpenGL::render_begin();
        wlr_renderer_begin(wf::get_core().renderer,
            output->handle->width, output->handle->height);
        wlr_output_render_software_cursors(output->handle,
            swap_damage.to_pixman());
        wlr_renderer_end(wf::get_core().renderer);
        OpenGL::render_end();

        /* Part 6: finalize frame: swap buffers, send frame_done, etc */
        OpenGL::unbind_output(output);
        output_damage->swap_buffers(swap_damage);
        swap_damage.clear();
        post_paint();
    }

    /**
     * Execute post-paint actions.
     */
    void post_paint()
    {
        effects->run_effects(OUTPUT_EFFECT_POST);

        if (constant_redraw_counter)
        {
            output_damage->schedule_repaint();
        }
    }

    void send_frame_done_recursive(wf::scene::node_ptr root,
        std::optional<wf::geometry_t> limit,
        const timespec& repaint_ended)
    {
        if (root->is_disabled())
        {
            return;
        }

        if (auto vnode = dynamic_cast<scene::view_node_t*>(root.get()))
        {
            for (auto& view : vnode->get_view()->enumerate_views())
            {
                if (!view->is_mapped())
                {
                    continue;
                }

                if (limit && !(view->get_bounding_box() & *limit))
                {
                    continue;
                }

                for (auto& child : view->enumerate_surfaces())
                {
                    child.surface->send_frame_done(repaint_ended);
                }
            }
        }

        for (auto& ch : root->get_children())
        {
            send_frame_done_recursive(ch, limit, repaint_ended);
        }
    }

    /**
     * Send frame_done to clients.
     */
    void send_frame_done()
    {
        timespec repaint_ended;
        clockid_t presentation_clock =
            wlr_backend_get_presentation_clock(wf::get_core_impl().backend);
        clock_gettime(presentation_clock, &repaint_ended);

        // TODO: during rendering, we should build a list of rendered surfaces and
        // send them all frame done instead of recalculating here.
        for (int i = 0; i < (int)wf::scene::layer::ALL_LAYERS; i++)
        {
            if (renderer)
            {
                auto limit = output->render->get_ws_box(
                    output->workspace->get_current_workspace());
                send_frame_done_recursive(output->node_for_layer(
                    (wf::scene::layer)i), limit, repaint_ended);
            } else
            {
                send_frame_done_recursive(output->node_for_layer(
                    (wf::scene::layer)i), {}, repaint_ended);
            }
        }
    }

    /* Workspace stream implementation */
    void workspace_stream_start(workspace_stream_t& stream)
    {
        stream.running = true;
        stream.scale_x = stream.scale_y = 1;

        /* damage the whole workspace region, so that we get a full repaint
         * when updating the workspace */
        output_damage->damage(output_damage->get_ws_box(stream.ws));
        workspace_stream_update(stream, 1, 1);
    }

    /**
     * Represents a surface together with its damage for the current frame
     */
    struct damaged_surface_t
    {
        wf::surface_interface_t *surface = nullptr;
        wf::point_t pos;
        wf::region_t damage;
    };

    using damaged_surface = std::unique_ptr<damaged_surface_t>;

    /**
     * Represents the state while calculating what parts of the output
     * to repaint
     */
    struct workspace_stream_repaint_t
    {
        std::vector<damaged_surface> to_render;
        wf::region_t ws_damage;
        wf::render_target_t fb;
    };

    /**
     * Calculate the damaged region of a simple wayfire_surface_t and
     * push it in the repaint list if needed.
     */
    void schedule_surface(workspace_stream_repaint_t& repaint,
        wf::surface_interface_t *surface, wf::point_t pos)
    {
        if (!surface->is_mapped())
        {
            return;
        }

        if (repaint.ws_damage.empty())
        {
            return;
        }

        auto ds = damaged_surface(new damaged_surface_t);
        wlr_box obox = {
            .x     = pos.x,
            .y     = pos.y,
            .width = surface->get_size().width,
            .height = surface->get_size().height
        };

        ds->damage = repaint.ws_damage & obox;
        if (!ds->damage.empty())
        {
            ds->pos     = pos;
            ds->surface = surface;

            /* Subtract opaque region from workspace damage. The views below
             * won't be visible, so no need to damage them */
            repaint.ws_damage ^= ds->surface->get_opaque_region(pos);
            repaint.to_render.push_back(std::move(ds));
        }
    }

    /**
     * Setup the stream, calculate damaged region, etc.
     */
    workspace_stream_repaint_t calculate_repaint_for_stream(
        workspace_stream_t& stream, float scale_x, float scale_y)
    {
        workspace_stream_repaint_t repaint;
        repaint.ws_damage = output_damage->get_ws_damage(stream.ws);

        /* we don't have to update anything */
        if (repaint.ws_damage.empty())
        {
            return repaint;
        }

        if ((scale_x != stream.scale_x) || (scale_y != stream.scale_y))
        {
            /* FIXME: enable scaled rendering */
        }

        OpenGL::render_begin();
        stream.buffer.allocate(output->handle->width, output->handle->height);
        OpenGL::render_end();

        repaint.fb = postprocessing->get_target_framebuffer();
        if ((stream.buffer.tex != 0))
        {
            /* Use the workspace buffers */
            repaint.fb.fb  = stream.buffer.fb;
            repaint.fb.tex = stream.buffer.tex;
        }

        auto g   = output->get_relative_geometry();
        auto cws = output->workspace->get_current_workspace();
        repaint.fb.geometry.x = (stream.ws.x - cws.x) * g.width,
        repaint.fb.geometry.y = (stream.ws.y - cws.y) * g.height;

        return repaint;
    }

    void clear_empty_areas(workspace_stream_repaint_t& repaint, wf::color_t color)
    {
        OpenGL::render_begin(repaint.fb);
        for (const auto& rect : repaint.ws_damage)
        {
            repaint.fb.logic_scissor(wlr_box_from_pixman_box(rect));
            OpenGL::clear(color, GL_COLOR_BUFFER_BIT);
        }

        OpenGL::render_end();
    }

    void render_views(workspace_stream_repaint_t& repaint)
    {
        for (auto& ds : wf::reverse(repaint.to_render))
        {
            ds->surface->simple_render(repaint.fb,
                ds->pos.x, ds->pos.y, ds->damage);

            if (ds->surface->get_wlr_surface() != nullptr)
            {
                wlr_presentation_surface_sampled_on_output(
                    wf::get_core_impl().protocols.presentation,
                    ds->surface->get_wlr_surface(), output->handle);
            }
        }
    }

    void workspace_stream_update(workspace_stream_t& stream,
        float scale_x = 1, float scale_y = 1)
    {
        workspace_stream_repaint_t repaint =
            calculate_repaint_for_stream(stream, scale_x, scale_y);

        if (repaint.ws_damage.empty())
        {
            return;
        }

        {
            stream_signal_t data(stream.ws, repaint.ws_damage, repaint.fb);
            output->render->emit_signal("workspace-stream-pre", &data);

            repaint.ws_damage  += wf::origin(output->get_layout_geometry());
            repaint.fb.geometry = repaint.fb.geometry + wf::origin(
                output->get_layout_geometry());

            scene::render_pass_begin_signal ev;
            ev.damage = repaint.ws_damage;
            ev.target = repaint.fb;
            wf::get_core().emit(&ev);
            repaint.ws_damage = ev.damage;
        }
        std::vector<wf::scene::render_instruction_t> instructions;
        for (auto& inst : output_damage->render_instances)
        {
            inst->schedule_instructions(instructions, repaint.fb, repaint.ws_damage);
        }

        if (stream.background.a < 0)
        {
            clear_empty_areas(repaint, background_color_opt);
        } else
        {
            clear_empty_areas(repaint, stream.background);
        }

        for (auto& instr : wf::reverse(instructions))
        {
            instr.instance->render(instr.target, instr.damage, output);
        }

        render_views(repaint);

        {
            stream_signal_t data(stream.ws, repaint.ws_damage, repaint.fb);
            output->render->emit_signal("workspace-stream-post", &data);
        }
    }

    void workspace_stream_stop(workspace_stream_t& stream)
    {
        stream.running = false;
    }
};

render_manager::render_manager(output_t *o) :
    pimpl(new impl(o))
{}
render_manager::~render_manager() = default;
void render_manager::set_renderer(render_hook_t rh)
{
    pimpl->set_renderer(rh);
}

void render_manager::set_redraw_always(bool always)
{
    pimpl->set_redraw_always(always);
}

wf::region_t render_manager::get_swap_damage()
{
    return pimpl->get_swap_damage();
}

void render_manager::schedule_redraw()
{
    pimpl->output_damage->schedule_repaint();
}

void render_manager::add_inhibit(bool add)
{
    pimpl->add_inhibit(add);
}

void render_manager::add_effect(effect_hook_t *hook, output_effect_type_t type)
{
    pimpl->effects->add_effect(hook, type);
}

void render_manager::rem_effect(effect_hook_t *hook)
{
    pimpl->effects->rem_effect(hook);
}

void render_manager::add_post(post_hook_t *hook)
{
    pimpl->postprocessing->add_post(hook);
}

void render_manager::rem_post(post_hook_t *hook)
{
    pimpl->postprocessing->rem_post(hook);
}

wf::region_t render_manager::get_scheduled_damage()
{
    return pimpl->output_damage->get_scheduled_damage();
}

void render_manager::damage_whole()
{
    pimpl->output_damage->damage_whole();
}

void render_manager::damage_whole_idle()
{
    pimpl->output_damage->damage_whole_idle();
}

void render_manager::damage(const wlr_box& box)
{
    pimpl->output_damage->damage(box);
}

void render_manager::damage(const wf::region_t& region)
{
    pimpl->output_damage->damage(region);
}

wlr_box render_manager::get_ws_box(wf::point_t ws) const
{
    return pimpl->output_damage->get_ws_box(ws);
}

wf::render_target_t render_manager::get_target_framebuffer() const
{
    return pimpl->postprocessing->get_target_framebuffer();
}

void render_manager::workspace_stream_start(workspace_stream_t& stream)
{
    pimpl->workspace_stream_start(stream);
}

void render_manager::workspace_stream_update(workspace_stream_t& stream,
    float scale_x, float scale_y)
{
    pimpl->workspace_stream_update(stream);
}

void render_manager::workspace_stream_stop(workspace_stream_t& stream)
{
    pimpl->workspace_stream_stop(stream);
}
} // namespace wf

/* End render_manager */
