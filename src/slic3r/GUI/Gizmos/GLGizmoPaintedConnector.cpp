#include "GLGizmoPaintedConnector.hpp"

#include "libslic3r/Model.hpp"
#include <array>
#include "libslic3r/Print.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <array>
#include <cassert>
#include <glad/gl.h>
#include <algorithm>
#include <string>
#include <utility>
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include <vector>
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include <imgui.h>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/Event.hpp"
#include <wx/busycursor.h>
#include "libslic3r/Color.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include <memory>
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "libslic3r/Connector/PaintedSurfaceConnector.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

namespace {
constexpr const char* GENERATED_CONNECTOR_VOLUME_NAME = "Painted Surface Connector";
bool is_generated_connector_volume(const ModelVolume* volume)
{
    return volume != nullptr && volume->name == GENERATED_CONNECTOR_VOLUME_NAME;
}
}

void GLGizmoPaintedConnector::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoPaintedConnector::on_get_name() const
{
    return _u8L("Painted Surface Connector");
}

bool GLGizmoPaintedConnector::on_init()
{

    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    m_desc["remove_all"]        = _L("Erase all");
    m_desc["circle"]            = _L("Circle");
    m_desc["sphere"]            = _L("Sphere");
    m_desc["pointer"]           = _L("Triangles");
    m_desc["tool_type"]         = _L("Tool type");
    m_desc["tool_brush"]        = _L("Brush");
    m_desc["tool_smart_fill"]   = _L("Smart fill");
    m_desc["cursor_size"]       = _L("Brush size");
    m_desc["paint_connector"]    = _L("Add connector paint");
    m_desc["paint_connector_caption"] = wxString();
    m_desc["erase_connector_caption"] = wxString();
    m_desc["erase_connector"] = _L("Remove connector paint");
    m_desc["smart_fill_angle"]  = _L("Smart fill angle");

    std::pair<wxString, wxString> paint_connector_shortcut    = {_L("Left mouse button"),         m_desc["paint_connector"]};
    std::pair<wxString, wxString> erase_connector_shortcut = {shift + _L("Left mouse button"), m_desc["erase_connector"]};

    m_shortcuts_brush = {
        paint_connector_shortcut,
        erase_connector_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]}
    };

    m_shortcuts_triangle = {
        paint_connector_shortcut, 
        erase_connector_shortcut
    };

    m_shortcuts_smart_fill = {
        paint_connector_shortcut,
        erase_connector_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]}
    };

    return true;
}

GLGizmoPaintedConnector::GLGizmoPaintedConnector(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

void GLGizmoPaintedConnector::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoPaintedConnector::render_tooltip_button(float x, float y)
{
    auto get_shortcuts = [this]() -> std::vector<std::pair<wxString, wxString>> {
        switch (m_tool_type) {
        case ToolType::BRUSH: return m_cursor_type == TriangleSelector::POINTER ? m_shortcuts_triangle : m_shortcuts_brush;

        case ToolType::SMART_FILL: return m_shortcuts_smart_fill;
        default: return {};
        }
    };

    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, get_shortcuts(), x, y);
}

void GLGizmoPaintedConnector::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    float  scale       = m_parent.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32


    const float approx_height = m_imgui->scaled(22.f);
    y = std::min(y, bottom_limit - approx_height);
    //BBS: GUI refactor: move gizmo to the right
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    //m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);

    // BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float cursor_slider_left     = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);

    const float cursor_type_radio_circle  = m_imgui->calc_text_size(m_desc["circle"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_sphere  = m_imgui->calc_text_size(m_desc["sphere"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_pointer = m_imgui->calc_text_size(m_desc["pointer"]).x + m_imgui->scaled(2.5f);

    const float buttons_width        = m_imgui->scaled(0.5f);
    const float minimal_slider_width = m_imgui->scaled(4.f);

    const float tool_type_radio_left       = m_imgui->calc_text_size(m_desc["tool_type"]).x + m_imgui->scaled(1.f);
    const float tool_type_radio_brush      = m_imgui->calc_text_size(m_desc["tool_brush"]).x + m_imgui->scaled(2.5f);
    const float tool_type_radio_smart_fill = m_imgui->calc_text_size(m_desc["tool_smart_fill"]).x + m_imgui->scaled(2.5f);

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const std::string t : {"paint_connector", "erase_connector"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max    += m_imgui->scaled(1.f);

    const float sliders_left_width = std::max(smart_fill_slider_left, cursor_slider_left);
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    float window_width = minimal_slider_width + sliders_left_width + slider_icon_width;
    const float empty_button_width = m_imgui->calc_button_size("").x;

    window_width                   = std::max(window_width, total_text_max);
    window_width                   = std::max(window_width, cursor_type_radio_circle + cursor_type_radio_sphere + cursor_type_radio_pointer);
    window_width                   = std::max(window_width, tool_type_radio_left + tool_type_radio_brush + tool_type_radio_smart_fill);
    window_width                   = std::max(window_width, 2.f * buttons_width + m_imgui->scaled(1.f));
    
    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["tool_type"]);

    std::array<wchar_t, 4> tool_ids;
    tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon };
    std::array<wchar_t, 4> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon };
    std::array<wxString, 4> tool_tips = { _L("Circle"), _L("Sphere"), _L("Triangle"), _L("Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        //std::string  str_label = std::string("");
        //std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));

        bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text         , ImVec4(1,1,1,1)); // ORCA Fixes icon rendered without colors while using Light theme
        ImGui::PushStyleColor(ImGuiCol_Button       , is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f,.6f,.6f,.2f)); // ORCA
        ImGui::PushStyleColor(ImGuiCol_ButtonActive , is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_Border       , is_active ? ImGuiWrapper::COL_ORCA        : ImVec4(0,0,0,0));         // ORCA
        ImGui::PushStyleColor(ImGuiCol_BorderActive , is_active ? ImGuiWrapper::COL_ORCA        : ImVec4(0,0,0,0));         // ORCA matched color for fixing flicker on click
        bool btn_clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f  * scale, 16.f  * scale)); // ORCA glyph_button for fixing unequal paddings
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        if (m_current_tool == ImGui::CircleButtonIcon)
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        else
            m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(cursor_slider_left);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + cursor_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else {
        assert(m_current_tool == ImGui::FillButtonIcon);
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["smart_fill_angle"]);
        std::string format_str = std::string("%.f") + I18N::translate_utf8("°",
                                                                            "Face angle threshold,"
                                                                            "placed after the number with no whitespace in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax,
                                            format_str.data(), 1.0f, true))
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    ImGui::Separator();

    render_tooltip_button(x, y);

    ImGui::SameLine();
    m_imgui->disabled_begin(mo->is_connector_painted() == false);
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset selection"), UndoRedo::SnapshotType::GizmoAction);
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part() && !is_generated_connector_volume(mv)) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    const bool has_generated_connectors = std::any_of(mo->volumes.begin(), mo->volumes.end(),
        [](const ModelVolume* volume) { return is_generated_connector_volume(volume); });
    m_imgui->disabled_begin(!mo->is_connector_painted() || mo->instances.size() != 1 || m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView);
    if (m_imgui->button(has_generated_connectors ? _L("Regenerate connectors") : _L("Generate connectors")))
        generate_connectors();
    m_imgui->disabled_end();

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({_L("Done")});
    if (m_imgui->button(_L("Done"))) {
        m_parent.reset_all_gizmos();
    }

    ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoPaintedConnector::update_model_object()
{
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part() || is_generated_connector_volume(mv))
            continue;

        ++idx;
        updated |= mv->connector_facets.set(*m_triangle_selectors[idx]);
    }

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoPaintedConnector::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    int volume_id = -1;
    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part() || is_generated_connector_volume(mv))
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        // Painted Surface Connector stores a single ENFORCER state: a higher state in the file is reset to NONE.
        m_triangle_selectors.back()->deserialize(mv->connector_facets.get_data(), false, EnforcerBlockerType::ENFORCER);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

PainterGizmoType GLGizmoPaintedConnector::get_painter_type() const
{
    return PainterGizmoType::CONNECTOR;
}

wxString GLGizmoPaintedConnector::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    return shift_down ? _L("Remove connector paint") : _L("Add connector paint");
}


void GLGizmoPaintedConnector::generate_connectors()
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || mo->instances.size() != 1 || m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView || m_triangle_selectors.empty())
        return;
 
    wxBusyCursor wait;
    const Selection& selection = m_parent.get_selection();
    const std::size_t instance_idx = selection.get_instance_idx();
    if (instance_idx >= mo->instances.size())
        return;

    const Transform3d object_to_world = mo->instances[instance_idx]->get_transformation().get_matrix();
    std::vector<Slic3r::Connector::Triangle> painted;
    std::size_t selector_idx = 0;

    for (const ModelVolume* volume : mo->volumes) {
        if (!volume->is_model_part() || is_generated_connector_volume(volume))
            continue;
        if (selector_idx >= m_triangle_selectors.size())
            return;

        const indexed_triangle_set selected = m_triangle_selectors[selector_idx++]->get_facets(EnforcerBlockerType::ENFORCER);
        const Transform3d volume_to_world = object_to_world * volume->get_matrix();
        const auto& vertices = selected.vertices;

        for (const Vec3i32& face : selected.indices) {
            const Vec3d a = volume_to_world * vertices[face(0)].cast<double>();
            const Vec3d b = volume_to_world * vertices[face(1)].cast<double>();
            const Vec3d c = volume_to_world * vertices[face(2)].cast<double>();
            painted.push_back({
                {a.x(), a.y(), a.z()},
                {b.x(), b.y(), b.z()},
                {c.x(), c.y(), c.z()}
            });
        }
    }

    if (painted.empty())
        return;

    const Slic3r::Connector::ConnectorResult generated =
        Slic3r::Connector::generate_connectors(painted);
    if (generated.triangles.empty())
        return;

    indexed_triangle_set connector_its;
    connector_its.vertices.reserve(generated.triangles.size() * 3);
    connector_its.indices.reserve(generated.triangles.size());
    const Transform3d world_to_object = object_to_world.inverse();

    for (const auto& triangle : generated.triangles) {
        const std::array<Slic3r::Connector::Vec3, 3> points = {triangle.a, triangle.b, triangle.c};
        const int first = static_cast<int>(connector_its.vertices.size());
        for (const auto& point : points) {
            const Vec3d world(point.x, point.y, point.z);
            connector_its.vertices.emplace_back((world_to_object * world).cast<float>());
        }
        connector_its.indices.emplace_back(first, first + 1, first + 2);
    }

    // TriangleMesh expects a shared indexed topology. The geometry core
    // emits independent triangles, so weld identical vertices and discard
    // any degenerate faces before constructing the printable volume.
    its_merge_vertices(connector_its);
    its_remove_degenerate_faces(connector_its);
    if (connector_its.indices.empty())
        return;

    TriangleMesh connector_mesh(std::move(connector_its));
    if (connector_mesh.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
        _u8L("Generate Painted Surface Connector"),
        UndoRedo::SnapshotType::GizmoAction);

    // Replace any previous generated connector only after the new mesh has
    // been generated successfully, so a failed regeneration keeps the old part.
    for (std::size_t i = mo->volumes.size(); i-- > 0;)
        if (is_generated_connector_volume(mo->volumes[i]))
            mo->delete_volume(i);

    ModelVolume* connector_volume = mo->add_volume(std::move(connector_mesh), ModelVolumeType::MODEL_PART, false);
    if (!connector_volume)
        return;

    connector_volume->name = GENERATED_CONNECTOR_VOLUME_NAME;
    connector_volume->set_type(ModelVolumeType::MODEL_PART);
    mo->invalidate_bounding_box();

    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    const auto it = std::find(objects.begin(), objects.end(), mo);
    if (it != objects.end())
        wxGetApp().obj_list()->update_info_items(std::distance(objects.begin(), it));

    // Refresh the ModelVolume list and Prepare viewport immediately after
    // adding the independent printable connector part.
    wxGetApp().plater()->update();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
}


} // namespace Slic3r::GUI
