#pragma once

struct wl_listener;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  class Server;
  class Workspace;
  class WorkspaceGroup;
  struct ResolvedWindowRule;

  // Helpers shared by the View implementation files.
  namespace view_detail {
    // Remove a listener that may never have been added, or was already removed.
    void removeListener(wl_listener& listener);
    // Direct child of the xdg scene tree that holds the toplevel subsurface tree.
    wlr_scene_node* toplevelSurfaceTreeNode(wlr_scene_tree* xdgTree, wlr_surface* mainSurface);
    WorkspaceGroup* windowRuleWorkspaceGroup(Server& server, const ResolvedWindowRule& rule, WorkspaceGroup* fallback);
    Workspace* windowRuleWorkspace(WorkspaceGroup* group, const ResolvedWindowRule& rule);
  } // namespace view_detail

} // namespace umbriel
