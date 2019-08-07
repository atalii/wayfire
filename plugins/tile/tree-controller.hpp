#ifndef WF_TILE_PLUGIN_TREE_CONTROLLER_HPP
#define WF_TILE_PLUGIN_TREE_CONTROLLER_HPP

#include "tree.hpp"
#include <config.hpp>

/* Contains functions which are related to manipulating the tiling tree */
namespace wf
{
class preview_indication_view_t;
namespace tile
{
/**
 * Run callback for each view in the tree
 */
void for_each_view(nonstd::observer_ptr<tree_node_t> root,
    std::function<void(wayfire_view)> callback);

/**
 * Restack the tiled views on the given output and workspace.
 * The goal is that all views appear together in the stacking list, and that
 * fullscreen views are always on the top of tiled non-fullscreen views.
 */
void restack_output_workspace(wf::output_t *output, wf_point workspace);

/**
 * Represents the current mode in which the tile plugin is.
 *
 * Invariant: while a controller is active, the tree structure shouldn't change,
 * except for changes by the controller itself.
 *
 * If such an external event happens, then controller will be destroyed.
 */
class tile_controller_t
{
  public:
    virtual ~tile_controller_t() = default;

    /** Called when the input is moved */
    virtual void input_motion(wf_point input) {}

    /**
     * Called when the input is released or the controller should stop
     * Note that a controller may be deleted without receiving input_released(),
     * in which case it should simply stop operation.
     */
    virtual void input_released() {}
};

/**
 * Represents the moving view action, i.e dragging a window to change its
 * position in the grid
 */
class move_view_controller_t : public tile_controller_t
{
  public:
    /**
     * Start the drag-to-reorder action.
     *
     * @param root The root of the tiling tree which is currently being
     *             manipulated
     * @param Where the grab has started
     */
    move_view_controller_t(std::unique_ptr<tree_node_t>& root,
        wf_point grab);
    ~move_view_controller_t();

    void input_motion(wf_point input) override;
    void input_released() override;

  protected:
    std::unique_ptr<tree_node_t>& root;
    nonstd::observer_ptr<view_node_t> grabbed_view;
    wf::output_t* output;
    wf_point current_input;

    nonstd::observer_ptr<wf::preview_indication_view_t> preview;
    /**
     * Create preview if it doesn't exist
     *
     * @param now The position of the input now. Used only if the preview
     *            needs to be created.
     */
    void ensure_preview(wf_point now);

    /**
     * Return the node under the input which is suitable for dropping on.
     */
    nonstd::observer_ptr<view_node_t> check_drop_destination(wf_point input);
};

class resize_view_controller_t : public tile_controller_t
{
  public:
   /**
     * Start the drag-to-resize action.
     *
     * @param root The root of the tiling tree which is currently being
     *             manipulated
     * @param Where the grab has started
     */
    resize_view_controller_t(std::unique_ptr<tree_node_t>& root,
        wf_point grab);
    ~resize_view_controller_t();

    void input_motion(wf_point input) override;

  protected:
    std::unique_ptr<tree_node_t>& root;

    /** Last input event location */
    wf_point last_point;

    /** Edges of the grabbed view that we're resizing */
    uint32_t resizing_edges;
    /** Calculate the resizing edges for the grabbing view. */
    uint32_t calculate_resizing_edges(wf_point point);

    /** The view we are resizing */
    nonstd::observer_ptr<view_node_t> grabbed_view;

    /*
     * A resizing pair of nodes is a pair of nodes we need to resize
     * The first one is always to the left/above the second one.
     */
    using resizing_pair_t = std::pair<nonstd::observer_ptr<tree_node_t>,
          nonstd::observer_ptr<tree_node_t>>;

    /** The horizontally-aligned pair we're resizing */
    resizing_pair_t horizontal_pair;
    /** The vertically-aligned pair we're resizing */
    resizing_pair_t vertical_pair;

    /*
     * Find a resizing pair in the given direction.
     *
     * The resizing pair depends on the currently grabbed view and the
     * resizing edges.
     */
    resizing_pair_t find_resizing_pair(bool horizontal);

    /**
     * Adjust the given positions and sizes while resizing.
     *
     * @param x1 The start of the first geometry
     * @param len1 The dimension of the first geometry
     * @param x2 The start of the second geometry, should be x1 + len1
     * @param len2 The length of the second geometry
     *
     * @param delta How much change to apply
     */
    void adjust_geometry(int32_t& x1, int32_t& len1,
        int32_t& x2, int32_t& len2, int32_t delta);
};

}
}


#endif /* end of include guard: WF_TILE_PLUGIN_TREE_CONTROLLER_HPP */

