#include "tetra_utils.hpp"

#include <prism/PrismCage.hpp>
#include <prism/geogram/AABB.hpp>
#include <prism/local_operations/remesh_pass.hpp>
#include <prism/local_operations/validity_checks.hpp>
#include <prism/spatial-hash/AABB_hash.hpp>
#include "AMIPS.h"
#include "prism/cgal/triangle_triangle_intersection.hpp"
#include "prism/energy/smoother_pillar.hpp"

#include <igl/boundary_facets.h>
#include <spdlog/fmt/bundled/ranges.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

void abort_and_debug(std::string msg = "")
{
    spdlog::dump_backtrace();
    assert(false && msg.c_str());
    throw std::runtime_error(msg);
}

void require(bool cond, std::string msg)
{
    if (!cond) abort_and_debug(msg);
}
auto set_inter = [](auto& A, auto& B) {
    std::vector<int> vec;
    std::set_intersection(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(vec));
    return vec;
};

auto set_insert = [](auto& A, auto& a) {
    auto it = std::lower_bound(A.begin(), A.end(), a);
    A.insert(it, a);
};

auto replace = [](auto& arr, auto a, auto b) {
    for (auto i = 0; i < arr.size(); i++) {
        if (arr[i] == a) {
            arr[i] = b;
            return i;
        }
    }
    assert(false);
    return -1;
};

auto id_in_array = [](auto& v, auto& k) {
    for (auto i = 0; i < v.size(); i++) {
        if (v[i] == k) return i;
    }
    return -1;
};

auto sorted_face = [](auto& conn, auto j) {
    auto face = Vec3i();
    for (auto k = 0; k < 3; k++) face[k] = conn[(1 + k + j) % 4];
    std::sort(face.begin(), face.end());
    return face;
};

auto attempt_shell_operation = prism::local_validity::attempt_zig_remesh;

namespace prism::tet {

auto compute_quality = [](const auto& vert_attrs, const auto& tets) -> double {
    auto maxqual = 0.;
    for (auto& t : tets) {
        auto q = tetra_quality(
            vert_attrs[t[0]].pos,
            vert_attrs[t[1]].pos,
            vert_attrs[t[2]].pos,
            vert_attrs[t[3]].pos);
        maxqual = std::max(maxqual, q);
    }
    return maxqual;
};

auto max_tetra_sizes = [](const auto& vert_attrs, const auto& tets) {
    auto max_size = 0.;
    for (auto& t : tets) {
        auto s = circumradi2(
            vert_attrs[t[0]].pos,
            vert_attrs[t[1]].pos,
            vert_attrs[t[2]].pos,
            vert_attrs[t[3]].pos);
        max_size = std::max(s, max_size);
    }
    return max_size;
};

std::tuple<
    std::vector<prism::tet::VertAttr>,
    std::vector<prism::tet::TetAttr>,
    std::vector<std::vector<int>>>
prepare_tet_info(
    const PrismCage& pc,
    const RowMatd& tet_v,
    const RowMati& tet_t,
    const Eigen::VectorXi& tet_v_pid)
{
    assert(tet_v_pid.size() == tet_v.rows());
    auto vert_info = [&tet_v, &pc, &tet_v_pid]() {
        std::vector<VertAttr> vert_attrs(tet_v.rows());
        auto nshell = pc.mid.size();
        for (auto i = 0; i < tet_v.rows(); i++) {
            vert_attrs[i].pos = tet_v.row(i);
            vert_attrs[i].mid_id = tet_v_pid[i];
            if (tet_v_pid[i] != -1) {
                assert(vert_attrs[i].pos == pc.mid[tet_v_pid[i]]);
            }
        }
        return vert_attrs;
    }();

    // Tet-Face (tet_t, k) -> Shell Prisms (pc.mF)
    auto tet_info = [&vert_info, &tet_t, &pc]() {
        std::vector<TetAttr> tet_attrs(tet_t.rows());

        std::map<std::array<int, 3>, int> cell_finder;
        for (auto i = 0; i < pc.F.size(); i++) {
            auto pri = pc.F[i];
            std::sort(pri.begin(), pri.end());
            cell_finder.emplace(pri, i);
        }
        for (auto i = 0; i < tet_t.rows(); i++) {
            auto& t_a = tet_attrs[i];
            for (auto j = 0; j < 4; j++) {
                t_a.conn[j] = tet_t(i, j);

                auto face = std::array<int, 3>();
                for (auto k = 0; k < 3; k++) face[k] = vert_info[tet_t(i, (j + k + 1) % 4)].mid_id;
                std::sort(face.begin(), face.end());
                if (face.front() < 0) continue; // not a boundary face.
                auto it = cell_finder.find(face);
                if (it != cell_finder.end()) { // found
                    t_a.prism_id[j] = it->second;
                }
            }
        }
        return tet_attrs;
    }();

    auto vert_tet_conn = [&tet_info, n_verts = vert_info.size()]() {
        auto vt_conn = std::vector<std::vector<int>>(n_verts);
        for (auto i = 0; i < tet_info.size(); i++) {
            for (auto j = 0; j < 4; j++) vt_conn[tet_info[i].conn[j]].emplace_back(i);
        }
        std::for_each(vt_conn.begin(), vt_conn.end(), [](auto& vec) {
            std::sort(vec.begin(), vec.end());
        });
        return vt_conn;
    }();

    // count how many marks
    auto count_marks = [&tet_info]() {
        auto cnt = 0;
        for (auto i = 0; i < tet_info.size(); i++) {
            for (auto j = 0; j < 4; j++) {
                if (tet_info[i].prism_id[j] != -1) cnt++;
            }
        }
        return cnt;
    };
    assert(count_marks() == pc.F.size());
    return std::tuple(vert_info, tet_info, vert_tet_conn);
};

/**
 * @brief
 *
 * @param pc
 * @param old_fid fill with -1
 * @param new_fid new prism id
 * @param new_conn
 * @param new_tracks
 */
void update_pc(
    PrismCage& pc,
    const std::vector<int>& old_fid,
    const std::vector<int>& new_fid,
    std::vector<Vec3i>& new_conn,
    const std::vector<std::set<int>>& new_tracks)
{
    assert(new_fid.size() == new_conn.size());
    assert(new_fid.size() == new_tracks.size());
    for (auto i : old_fid) pc.F[i].fill(-1);
    prism::local_validity::triangle_shifts(new_conn);
    for (auto i = 0; i < new_fid.size(); i++) {
        auto f = new_fid[i];
        if (f >= pc.F.size()) {
            pc.F.resize(f + 1);
            pc.track_ref.resize(f + 1);
        }
        pc.F[f] = new_conn[i];
        pc.track_ref[f] = new_tracks[i];
    }

    if (pc.top_grid != nullptr) {
        spdlog::trace("HashGrid Update");
        for (auto f : old_fid) {
            pc.top_grid->remove_element(f);
            pc.base_grid->remove_element(f);
        }
        pc.top_grid->insert_triangles(pc.top, pc.F, new_fid);
        pc.base_grid->insert_triangles(pc.base, pc.F, new_fid);
    }
}

std::vector<int> edge_adjacent_boundary_face(
    const std::vector<TetAttr>& tet_attrs,
    const std::vector<std::vector<int>>& vert_conn,
    int v0,
    int v1)
{
    auto& nb0 = vert_conn[v0];
    auto& nb1 = vert_conn[v1];
    auto affected = set_inter(nb0, nb1); // removed
    std::vector<int> bnd_pris;
    for (auto t : affected) {
        for (auto j = 0; j < 4; j++) {
            if (tet_attrs[t].prism_id[j] != -1 // boundary
                && tet_attrs[t].conn[j] != v0 //  contains v0 AND v1
                && tet_attrs[t].conn[j] != v1) {
                bnd_pris.push_back(tet_attrs[t].prism_id[j]);
            }
        }
    }
    return bnd_pris;
};


void update_tetra_conn(
    const std::vector<VertAttr>& vert_attrs,
    std::vector<TetAttr>& tet_attrs,
    std::vector<std::vector<int>>& vert_conn,
    const std::vector<int>& affected,
    const std::vector<Vec4i>& new_tets,
    const std::vector<int>& modified_pids,
    const std::vector<Vec3i>& modified_tris)
{
    // update connectivity: VT
    vert_conn.resize(vert_attrs.size());

    // resetting prism_id.
    std::map<Vec3i, int> moved_pris_assigner;
    for (auto ti : affected) {
        tet_attrs[ti].is_removed = true;
        for (auto j = 0; j < 4; j++) {
            auto pid = tet_attrs[ti].prism_id[j];
            if (pid != -1) {
                auto face = Vec3i();
                for (auto k = 0; k < 3; k++)
                    face[k] = vert_attrs[tet_attrs[ti].conn[(j + k + 1) % 4]].mid_id;
                std::sort(face.begin(), face.end());
                assert(face.front() != -1);
                moved_pris_assigner.emplace(face, pid);
            }
        }
    }
    assert(modified_pids.size() == modified_tris.size());
    for (auto i = 0; i < modified_pids.size(); i++) {
        auto t = modified_tris[i];
        std::sort(t.begin(), t.end());
        // assert(moved_pris_assigner.find(t) == moved_pris_assigner.end());
        moved_pris_assigner.insert_or_assign(t, modified_pids[i]);
    }
    spdlog::trace("sorted moved tris {}", moved_pris_assigner);
    spdlog::trace("new pid num {}", modified_pids.size());
    auto cnt_assigned_prisms = 0;

    // remove tets from VT.
    std::set<int> affected_verts;
    for (auto& t : affected)
        for (auto i = 0; i < 4; i++) {
            affected_verts.insert(tet_attrs[t].conn[i]);
        }
    for (auto v : affected_verts) {
        auto diff = std::vector<int>();
        set_minus(vert_conn[v], affected, diff);
        vert_conn[v] = std::move(diff);
    }

    auto n_tet = tet_attrs.size();
    for (auto& t : new_tets) {
        for (auto i = 0; i < t.size(); i++) {
            auto diff = std::vector<int>();
            set_minus(vert_conn[t[i]], affected, diff);
            vert_conn[t[i]] = std::move(diff);

            set_insert(vert_conn[t[i]], n_tet);
        }
        n_tet++;
        auto t_a = TetAttr();
        t_a.conn = t;

        // if face in t is a `moved_tris`, get the corresponding index (oppo vert).
        auto tet_mid = Vec4i();
        for (auto j = 0; j < 4; j++) {
            tet_mid[j] = vert_attrs[t[j]].mid_id;
        }
        spdlog::trace("tet {} tetmid {}", t_a.conn, tet_mid);
        for (auto j = 0; j < 4; j++) {
            auto face = sorted_face(tet_mid, j);
            if (face.front() == -1) {
                continue;
            }
            auto it = moved_pris_assigner.find(face);
            if (it != moved_pris_assigner.end()) {
                t_a.prism_id[j] = it->second;
                cnt_assigned_prisms++;
            }
        }

        tet_attrs.emplace_back(t_a);
    }

    spdlog::trace(
        "sorted moved tris number {} -> assigned {}",
        moved_pris_assigner.size(),
        cnt_assigned_prisms);
    // assert(cnt_assigned_prisms == sorted_moved_tris.size() && "All assigned");
    assert(n_tet == tet_attrs.size());
    require(modified_tris.size() <= cnt_assigned_prisms, "Some new prisms are not assigned!");
    if (modified_tris.empty()) { // internal
        require(
            moved_pris_assigner.size() == cnt_assigned_prisms,
            "Internal edge should not lose any tag.");
    }
}


bool split_edge(
    PrismCage& pc,
    prism::local::RemeshOptions& option,
    std::vector<VertAttr>& vert_attrs,
    std::vector<TetAttr>& tet_attrs,
    std::vector<std::vector<int>>& vert_conn,
    int v0,
    int v1)
{
    auto& nb0 = vert_conn[v0];
    auto& nb1 = vert_conn[v1];
    auto affected = set_inter(nb0, nb1); // removed
    assert(!affected.empty());
    spdlog::debug("Splitting... {} {}", v0, v1);

    std::vector<Vec4i> new_tets;
    std::vector<TetAttr> new_attrs;
    auto vx = vert_attrs.size();


    auto bnd_pris = edge_adjacent_boundary_face(tet_attrs, vert_conn, v0, v1);
    for (auto t : affected) {
        new_tets.push_back(tet_attrs[t].conn);
        new_attrs.push_back(tet_attrs[t]);
        replace(new_tets.back(), v0, vx);

        new_tets.push_back(tet_attrs[t].conn);
        new_attrs.push_back(tet_attrs[t]);
        replace(new_tets.back(), v1, vx);
    }

    vert_attrs.push_back({});
    vert_attrs.back().pos = ((vert_attrs[v0].pos + vert_attrs[v1].pos) / 2);
    spdlog::trace("{}& {} -> {}", vert_attrs[v0].pos, vert_attrs[v1].pos, vert_attrs.back().pos);
    auto rollback = [&]() {
        vert_attrs.pop_back();
        pc.top.pop_back();
        pc.base.pop_back();
        pc.mid.pop_back();
        return false;
    };

    auto p_vx = pc.mid.size();
    auto pv0 = vert_attrs[v0].mid_id, pv1 = vert_attrs[v1].mid_id;
    if (!bnd_pris.empty()) {
        spdlog::trace("Handling boundary edge with pris {}", bnd_pris);
        assert(bnd_pris.size() == 2);
        vert_attrs.back().mid_id = p_vx;
        assert(vert_attrs[v0].mid_id >= 0 && vert_attrs[v1].mid_id >= 0);
        pc.top.push_back((pc.top[pv0] + pc.top[pv1]) / 2);
        pc.base.push_back((pc.base[pv0] + pc.base[pv1]) / 2);
        pc.mid.push_back(vert_attrs.back().pos);
        option.target_adjustment.push_back(
            (option.target_adjustment[pv0] + option.target_adjustment[pv1]) / 2);
        spdlog::trace("pusher {} {}", pc.top.back(), pc.base.back());
    }

    for (auto t : new_tets) { // Vec4i
        if (!tetra_validity(vert_attrs, t)) return rollback();
    }

    std::vector<int> new_fid;
    std::vector<Vec3i> moved_tris;
    if (!bnd_pris.empty()) {
        auto pvx = pc.mid.size() - 1;
        assert(bnd_pris.size() == 2);
        auto& F = pc.F;
        auto f0 = bnd_pris.front(), f1 = bnd_pris.back();
        std::vector<int> old_fids = {f0, f1};
        moved_tris = std::vector<Vec3i>{F[f0], F[f1], F[f0], F[f1]};
        spdlog::trace("pvx {}", pvx);
        replace(moved_tris[0], pv0, pvx);
        replace(moved_tris[1], pv0, pvx);
        replace(moved_tris[2], pv1, pvx);
        replace(moved_tris[3], pv1, pvx);

        std::vector<std::set<int>> new_tracks;
        std::vector<RowMatd> local_cp;
        auto flag = attempt_shell_operation(
            pc,
            pc.track_ref,
            option,
            -1,
            old_fids,
            moved_tris,
            new_tracks,
            local_cp);
        if (flag != 0) return rollback();

        // distribute and assign new_tracks.
        new_fid = {f0, f1, int(F.size()), int(F.size() + 1)};
        update_pc(pc, {f0, f1}, new_fid, moved_tris, new_tracks);
    }

    update_tetra_conn(vert_attrs, tet_attrs, vert_conn, affected, new_tets, new_fid, moved_tris);
    assert([&]() -> bool {
        auto vx = vert_conn.size() - 1;
        spdlog::trace("vx {}", vx);
        return true;
    }());

    if (!tetmesh_sanity(tet_attrs, vert_attrs, vert_conn, pc)) abort_and_debug("Sanity Error");

    return true;
}

auto get_newton_position = [](const auto& vert_attrs,
                              const auto& tet_attrs,
                              const auto& nb,
                              int v0,
                              const Vec3d& old_pos) {
    auto orient_preserve_tet_reorder = [](const auto& conn, auto v0) {
        auto vl_id = id_in_array(conn, v0);
        assert(vl_id != -1);
        auto reorder = std::array<std::array<size_t, 4>, 4>{
            {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 1, 0, 2}}};
        auto newconn = conn;
        for (auto j = 0; j < 4; j++) newconn[j] = conn[reorder[vl_id][j]];
        return newconn;
    };
    // build assembles with v0 in the front.
    std::vector<std::array<double, 12>> assembles(nb.size());
    auto iter_id = 0;
    for (auto t : nb) {
        auto& T = assembles[iter_id];
        auto local_verts = orient_preserve_tet_reorder(tet_attrs[t].conn, v0);

        for (auto i = 0; i < 4; i++) {
            for (auto j = 0; j < 3; j++) {
                T[i * 3 + j] = vert_attrs[local_verts[i]].pos[j];
            }
        }
        iter_id++;
    }

    return newton_position_from_stack(assembles);
};

auto get_snap_position(const PrismCage& pc, const std::vector<int>& neighbor_pris, int v0)
{
    auto query = [&ref = pc.ref](const Vec3d& s, const Vec3d& t, const std::set<int>& total_trackee)
        -> std::optional<Vec3d> {
        std::array<Vec3d, 2> seg_query{s, t};
        for (auto f : total_trackee) {
            auto v0 = ref.F(f, 0), v1 = ref.F(f, 1), v2 = ref.F(f, 2);
            auto mid_intersect = prism::cgal::segment_triangle_intersection(
                seg_query,
                {ref.V.row(v0), ref.V.row(v1), ref.V.row(v2)});
            if (mid_intersect) return mid_intersect;
        }
        return {};
    };
    std::set<int> total_trackee;
    for (auto f : neighbor_pris)
        total_trackee.insert(pc.track_ref[f].begin(), pc.track_ref[f].end());
    auto mid_intersect = query(pc.top[v0], pc.base[v0], total_trackee);
    // assert(mid_intersect && "Snap project should succeed.");
    return mid_intersect;
}

bool smooth_vertex(
    PrismCage& pc,
    const prism::local::RemeshOptions& option,
    std::vector<VertAttr>& vert_attrs,
    const std::vector<TetAttr>& tet_attrs,
    const std::vector<std::vector<int>>& vert_conn,
    SmoothType smooth_type,
    int v0,
    double size_control)
{

    auto& tet_nb = vert_conn[v0];
    const Vec3d old_pos = vert_attrs[v0].pos;
    auto fmax = Eigen::Map<RowMati>(pc.F[0].data(), pc.F.size(), 3).maxCoeff();
    auto neighbor_pris = std::vector<int>();
    const auto pv0 = vert_attrs[v0].mid_id;
    for (auto t : tet_nb) {
        for (auto j = 0; j < 4; j++) {
            auto pid = tet_attrs[t].prism_id[j];
            if (pid != -1 && tet_attrs[t].conn[j] != v0) {
                neighbor_pris.push_back(pid);
                assert(pid < pc.F.size());
            }
        }
    }

    const auto old_pillar = pv0 != -1 ? std::tie(pc.base[pv0], pc.mid[pv0], pc.top[pv0])
                                      : std::tuple<Vec3d, Vec3d, Vec3d>();

    auto rollback = [&]() {
        vert_attrs[v0].pos = old_pos;
        if (pv0 != -1) std::tie(pc.base[pv0], pc.mid[pv0], pc.top[pv0]) = old_pillar;
        return false;
    };
    if (smooth_type == SmoothType::kInteriorNewton) {
        assert(neighbor_pris.empty());
        vert_attrs[v0].pos = get_newton_position(vert_attrs, tet_attrs, tet_nb, v0, old_pos);
    } else {
        assert(pv0 != -1);
        assert(!neighbor_pris.empty());

        auto nbi = std::vector<int>(neighbor_pris.size());
        for (auto i = 0; i < nbi.size(); i++) nbi[i] = id_in_array(pc.F[neighbor_pris[i]], pv0);
        bool snap_mid =
            (smooth_type == SmoothType::kSurfaceSnap || smooth_type == SmoothType::kShellPan);
        if (smooth_type == SmoothType::kShellPan) {
            auto new_direction = prism::smoother_direction(
                pc.base,
                pc.mid,
                pc.top,
                pc.F,
                pc.ref.aabb->num_freeze,
                neighbor_pris,
                nbi,
                pv0);
            if (!new_direction) {
                spdlog::trace("No better location.");
                return rollback();
            }

            std::tie(pc.base[pv0], pc.mid[pv0], pc.top[pv0]) = std::tie(
                pc.base[pv0] + new_direction.value(),
                pc.mid[pv0] + new_direction.value(),
                pc.top[pv0] + new_direction.value());
            // followed by snap.
        }
        if (snap_mid) {
            auto snapped = get_snap_position(pc, neighbor_pris, pv0);
            if (!snapped) {
                assert(smooth_type != SmoothType::kSurfaceSnap);
                spdlog::trace("No pan.");
                return rollback();
            }
            vert_attrs[v0].pos = snapped.value();
            pc.mid[pv0] = vert_attrs[v0].pos;
        } else {
            assert(
                smooth_type == SmoothType::kShellZoom || smooth_type == SmoothType::kShellRotate);
            auto func = prism::zoom;
            if (smooth_type == SmoothType::kShellRotate) func = prism::rotate;
            auto great_prism =
                func(pc.base, pc.mid, pc.top, pc.F, neighbor_pris, nbi, pv0, option.target_thickness);
            if (!great_prism) {
                spdlog::trace("No better prism.");
                return rollback();
            }
            std::tie(pc.base[pv0], pc.top[pv0]) = great_prism.value();
        }
    }


    if (pv0 == -1) {
        // Consider size only when internal. TODO: figure out priority of snapping vs size.
        auto old_tets = std::vector<Vec4i>(tet_nb.size());
        for (auto i = 0; i < tet_nb.size(); i++) old_tets[i] = tet_attrs[tet_nb[i]].conn;
        auto after_size = max_tetra_sizes(vert_attrs, old_tets);
        if (after_size > size_control) return rollback();
    }
    for (auto ti : tet_nb) {
        auto& t = tet_attrs[ti].conn;
        if (!tetra_validity(vert_attrs, t)) {
            return rollback();
        }
    }

    if (pv0 != -1) { // boundary, shell attempt
        auto old_fids = std::vector<int>();
        for (auto t : tet_nb) {
            auto& t_a = tet_attrs[t];
            for (auto j = 0; j < 4; j++) {
                if (t_a.prism_id[j] != -1 && t_a.conn[j] != v0) { // not opposite
                    old_fids.push_back(t_a.prism_id[j]);
                }
            }
        }
        assert(!old_fids.empty());
        auto moved_tris = std::vector<Vec3i>();
        for (auto f : old_fids) moved_tris.emplace_back(pc.F[f]);

        std::vector<std::set<int>> new_tracks;
        std::vector<RowMatd> local_cp;
        auto flag = attempt_shell_operation(
            pc,
            pc.track_ref,
            option,
            1e10, // TODO: smoothing tet already captured energy, no guard here.
            old_fids,
            moved_tris,
            new_tracks,
            local_cp);
        if (flag != 0) return rollback();
        update_pc(pc, old_fids, old_fids, moved_tris, new_tracks);
    }

    spdlog::trace("Vertex Snapped!!");
    // not modifying connectivity.
    return true;
}

bool tetmesh_sanity(
    const std::vector<TetAttr>& tet_attrs,
    const std::vector<VertAttr>& vert_attrs,
    const std::vector<std::vector<int>>& vert_tet_conn,
    const PrismCage& pc)
{
    for (auto& tet : tet_attrs) {
        if (tet.is_removed) continue;
        if (!tetra_validity(vert_attrs, tet.conn)) {
            spdlog::critical("Invalid Tet {}", tet.conn);
            return false;
        }
    }

    // duplicate tets
    auto tet_duplicate = std::set<Vec4i>();
    auto face_duplicate = std::map<Vec3i, int>();
    for (auto& tet : tet_attrs) {
        if (tet.is_removed) continue;
        {
            auto conn = tet.conn;
            std::sort(conn.begin(), conn.end());
            auto it = tet_duplicate.find(conn);
            if (it == tet_duplicate.end())
                tet_duplicate.insert(conn);
            else {
                spdlog::critical("duplicate tet {}", tet.conn);
                return false;
            }
        }
        for (auto j = 0; j < 4; j++) {
            auto face = sorted_face(tet.conn, j);
            auto it = face_duplicate.find(face);
            if (it == face_duplicate.end())
                face_duplicate.emplace(face, 1);
            else {
                it->second++;
                if (it->second > 2) {
                    spdlog::critical("Duplicate face {}", face);
                    return false;
                }
            }
        }
    }

    // corresponding prism id
    auto prism_faces = std::set<Vec3i>();
    auto boundary_faces = std::set<Vec3i>();
    for (auto& tet : tet_attrs) {
        if (tet.is_removed) continue;
        for (auto j = 0; j < 4; j++) {
            if (tet.prism_id[j] != -1) {
                auto face = sorted_face(tet.conn, j);
                boundary_faces.insert(face);
            }
        }
    }
    auto count_pc_faces = [](const auto& pc) {
        auto cnt = 0;
        for (auto f : pc.F) {
            if (f[0] == -1) continue;
            cnt++;
        }
        return cnt;
    };
    if (boundary_faces.size() != count_pc_faces(pc)) {
        spdlog::critical("Miss referenced prism.");
        return false;
    }

    // check internal vertices has neighbor
    for (auto i = 0; i < vert_attrs.size(); i++) {
        if (vert_attrs[i].mid_id >= 0) continue;
        auto& nb = (vert_tet_conn[i]);
        if (nb.empty()) continue;


        std::vector<Vec4i> old_tets(nb.size());
        for (auto k = 0; k < nb.size(); k++) old_tets[k] = tet_attrs[nb[k]].conn;
        RowMati bF;
        igl::boundary_facets(Eigen::Map<RowMati>(old_tets[0].data(), old_tets.size(), 4), bF);
        for (auto k = 0; k < bF.size(); k++)
            if (*(bF.data() + k) == i) {
                spdlog::critical("Internal vert on boundary!");
                return false;
            };
    }

    // check that mid is that same as pos
    for (auto i = 0; i < vert_attrs.size(); i++) {
        auto& v = vert_attrs[i];
        if (v.mid_id != -1)
            if (v.pos != pc.mid[v.mid_id]) {
                spdlog::critical(
                    "Pos for {} ({}) not equal mid {} {}",
                    i,
                    pc.mid[v.mid_id],
                    v.mid_id,
                    pc.mid[v.mid_id]);
                return false;
            }
    }
    return true;
}

bool collapse_edge(
    PrismCage& pc,
    const prism::local::RemeshOptions& option,
    std::vector<VertAttr>& vert_attrs,
    std::vector<TetAttr>& tet_attrs,
    std::vector<std::vector<int>>& vert_conn,
    int v1_id,
    int v2_id,
    double size_control)
{
    spdlog::debug(
        "Tet, Collapsing ({})->{}, with mid {}->{}",
        v1_id,
        v2_id,
        vert_attrs[v1_id].mid_id,
        vert_attrs[v2_id].mid_id); // erasing v1_id

    auto& nb1 = vert_conn[v1_id];
    auto& nb2 = vert_conn[v2_id];
    auto affected = nb1;

    assert(!set_inter(nb1, nb2).empty());
    if (vert_attrs[v1_id].mid_id != -1 && vert_attrs[v2_id].mid_id == -1) {
        return false; // TODO: erase v1, and assign its prism tracker to v2.
    }

    auto bnd_faces = edge_adjacent_boundary_face(tet_attrs, vert_conn, v1_id, v2_id);
    if (bnd_faces.empty() && (vert_attrs[v1_id].mid_id != -1 && vert_attrs[v2_id].mid_id != -1)) {
        spdlog::trace("Internal edge connecting boundary vertices.");
        return false;
    }
    std::vector<Vec4i> old_tets(affected.size());
    for (auto i = 0; i < affected.size(); i++) old_tets[i] = tet_attrs[affected[i]].conn;
    auto before_quality = compute_quality(vert_attrs, old_tets);

    std::vector<int> old_tids = nb1;
    std::vector<Vec4i> new_tets;
    auto cnt_newtid = 0;

    for (auto t : old_tids) {
        auto line_tet = tet_attrs[t].conn;
        auto v1_i = id_in_array(line_tet, v1_id);
        auto v2_i = id_in_array(line_tet, v2_id);
        assert(v1_i >= 0);
        if (v2_i >= 0) continue; // the tets containing both are deleted.
        line_tet[v1_i] = v2_id;
        new_tets.emplace_back(line_tet);
        cnt_newtid++;
    }
    assert(cnt_newtid < old_tids.size());
    spdlog::trace("Old tets : {}", old_tets);
    spdlog::trace("New tets : {}", new_tets);
    auto rollback = []() { return false; };
    { // link condition TODO: HACK: not kosher
        auto old_verts = std::set<int>();
        auto new_verts = std::set<int>();
        for (auto t : old_tets) old_verts.insert(t.begin(), t.end());
        for (auto t : new_tets) new_verts.insert(t.begin(), t.end());
        if (new_verts.size() != old_verts.size() - 1) {
            spdlog::debug("Violated link condition");
            return false;
        }
    }

    auto after_quality = compute_quality(vert_attrs, new_tets);
    if (after_quality > option.collapse_quality_threshold && before_quality < after_quality)
        return false;
    auto after_size = max_tetra_sizes(vert_attrs, new_tets);
    if (after_size > size_control) return false;
    for (auto t : new_tets) {
        if (!tetra_validity(vert_attrs, t)) return false;
    }
    for (auto f : bnd_faces) {
        assert(pc.F[f][0] != -1);
    }
    auto old_fid = std::vector<int>();
    auto new_fid = std::vector<int>();
    auto moved_tris = std::vector<Vec3i>();
    auto nb_pids = [&vert_conn, &tet_attrs](auto v_id) {
        auto nb = std::vector<int>();
        for (auto t : vert_conn[v_id]) {
            // spdlog::trace("conn {} \t {}", tet_attrs[t].conn, tet_attrs[t].prism_id);
            assert(tet_attrs[t].is_removed == false);
            for (auto j = 0; j < 4; j++) {
                auto pid = tet_attrs[t].prism_id[j];
                if (tet_attrs[t].conn[j] != v_id && pid != -1) {
                    // a surface adjacent to v1.
                    nb.push_back(pid);
                }
            }
        }
        std::sort(nb.begin(), nb.end());
        return nb;
    };

    if (!bnd_faces.empty()) {
        auto neighbor0 = nb_pids(v1_id), neighbor1 = nb_pids(v2_id);
        // spdlog::trace("NB0 {} NB1 {}", neighbor0, neighbor1);
        std::tie(old_fid, new_fid, moved_tris) = [&neighbor0,
                                                  &neighbor1,
                                                  &F = pc.F,
                                                  &u1 = vert_attrs[v2_id].mid_id,
                                                  &u0 = vert_attrs[v1_id].mid_id]() {
            assert(u1 >= 0 && u0 >= 0);
            std::vector<Vec3i> moved_tris;
            moved_tris.reserve(neighbor0.size() + neighbor1.size() - 4);

            std::vector<int> new_fid, old_fid;
            for (auto f : neighbor0) {
                auto new_tris = F[f];
                spdlog::trace("newtris [{},{},{}],", new_tris[0], new_tris[1], new_tris[2]);
                assert(new_tris[0] != -1);
                old_fid.push_back(f);
                if (id_in_array(new_tris, u1) != -1) continue; // collapsed faces
                replace(new_tris, u0, u1);
                moved_tris.emplace_back(new_tris);
                new_fid.push_back(f);
            }
            assert(old_fid.size() == new_fid.size() + 2);
            spdlog::trace("Shell u0,u1 {}, {}", u0, u1);

            return std::tuple(old_fid, new_fid, moved_tris);
        }();
        // spdlog::trace("moved tris {}", moved_tris);
        std::vector<std::set<int>> new_tracks;
        std::vector<RowMatd> local_cp;
        auto flag = attempt_shell_operation(
            pc,
            pc.track_ref,
            option,
            -1,
            old_fid,
            moved_tris,
            new_tracks,
            local_cp);
        if (flag != 0) return rollback();

        update_pc(pc, old_fid, new_fid, moved_tris, new_tracks);
        vert_attrs[v1_id].mid_id = vert_attrs[v2_id].mid_id;
    }

    vert_conn[v1_id].clear();
    update_tetra_conn(vert_attrs, tet_attrs, vert_conn, affected, new_tets, new_fid, moved_tris);

    vert_attrs[v1_id].pos.fill(0);
    vert_attrs[v1_id].mid_id = -1;

    if (!tetmesh_sanity(tet_attrs, vert_attrs, vert_conn, pc)) abort_and_debug("Sanity Error");

    return true;
}

bool swap_edge(
    const PrismCage& pc,
    const prism::local::RemeshOptions& option,
    std::vector<VertAttr>& vert_attrs,
    std::vector<TetAttr>& tet_attrs,
    std::vector<std::vector<int>>& vert_conn,
    int v1_id,
    int v2_id,
    double size_control)
{
    // 3-2 edge to face.
    auto& nb1 = vert_conn[v1_id];
    auto& nb2 = vert_conn[v2_id];
    auto affected = set_inter(nb1, nb2);
    assert(!affected.empty());
    if (affected.size() != 3) {
        spdlog::trace("Affect Neighbor Too Many {} (>2)", affected.size());
        return false;
    }

    auto bnd_faces = edge_adjacent_boundary_face(tet_attrs, vert_conn, v1_id, v2_id);
    if (!bnd_faces.empty()) return false; // NOT handling boundary edges for now.

    std::vector<Vec4i> old_tets(affected.size());
    for (auto i = 0; i < affected.size(); i++) old_tets[i] = tet_attrs[affected[i]].conn;
    auto before_quality = compute_quality(vert_attrs, old_tets);
    auto new_tets = [&tet_attrs, v1_id, v2_id, &affected]() {
        auto t0_id = affected[0];
        auto t1_id = affected[1];
        auto t2_id = affected[2];
        auto n0_id = -1, n1_id = -1, n2_id = -1;
        for (int j = 0; j < 4; j++) {
            auto v0j = tet_attrs[t0_id].conn[j];
            if (v0j != v1_id && v0j != v2_id) {
                if (id_in_array(tet_attrs[t1_id].conn, v0j) != -1) n1_id = v0j;
                if (id_in_array(tet_attrs[t2_id].conn, v0j) != -1) n2_id = v0j;
            }
            if (id_in_array(tet_attrs[t0_id].conn, tet_attrs[t1_id].conn[j]) == -1)
                n0_id = tet_attrs[t1_id].conn[j];
        }
        assert(n0_id != n1_id && n1_id != n2_id);
        // T0 = (n1,n2,v1,v2) -> (n1,n2,v1,n0)
        // T1 = (n0, n1, v1,v2) ->  (n0, n1, n2,v2)
        // T2 = (n0,n2, v1,v2) -> (-1,-1,-1,-1)
        auto new_tets = std::vector<Vec4i>(2);
        auto new_tids = std::vector<int>({t0_id, t1_id});
        auto replace = [](auto& arr, auto v0, auto v1) {
            for (auto j = 0; j < arr.size(); j++)
                if (arr[j] == v0) arr[j] = v1;
        };
        new_tets[0] = tet_attrs[new_tids[0]].conn;
        new_tets[1] = tet_attrs[new_tids[1]].conn;

        replace(new_tets[0], v2_id, n0_id);
        replace(new_tets[1], v1_id, n2_id);
        return new_tets;
    }();

    auto after_size = max_tetra_sizes(vert_attrs, new_tets);
    if (after_size > size_control) return false;
    auto after_quality = compute_quality(vert_attrs, new_tets);
    if (before_quality < after_quality) return false;
    for (auto t : new_tets) {
        if (!tetra_validity(vert_attrs, t)) return false;
    }

    update_tetra_conn(vert_attrs, tet_attrs, vert_conn, affected, new_tets, {}, {});

    if (!tetmesh_sanity(tet_attrs, vert_attrs, vert_conn, pc)) abort_and_debug("Sanity Error");
    return true;
}


// 2-3, internal only.
bool swap_face(
    const PrismCage& pc,
    const prism::local::RemeshOptions& option,
    std::vector<VertAttr>& vert_attrs,
    std::vector<TetAttr>& tet_attrs,
    std::vector<std::vector<int>>& vert_conn,
    int v0_id,
    int v1_id,
    int v2_id,
    double size_control)
{
    auto& nb0 = vert_conn[v0_id];
    auto& nb1 = vert_conn[v1_id];
    auto& nb2 = vert_conn[v2_id];
    auto inter0_1 = set_inter(nb0, nb1);
    auto affected = set_inter(inter0_1, nb2);
    if (affected.size() != 2) { // has to be on boundary, or an invalid face input.
        assert(edge_adjacent_boundary_face(tet_attrs, vert_conn, v0_id, v1_id).size() > 0);
        return false;
    }

    std::vector<Vec4i> old_tets(affected.size());
    for (auto i = 0; i < affected.size(); i++) old_tets[i] = tet_attrs[affected[i]].conn;
    auto before_quality = compute_quality(vert_attrs, old_tets);
    // no top/bottom ordering of the two tets are assumed.
    auto t0 = affected.front(), t1 = affected.back();
    auto find_other = [](const Vec4i& tet, const Vec3i& tri) {
        for (auto j = 0; j < 4; j++) {
            auto ids = id_in_array(tri, tet[j]);
            if (ids == -1) {
                return tet[j];
            }
        }
        assert(false);
        return -1;
        // reorient
        // return true;
    };
    auto u0 = find_other(tet_attrs[t0].conn, {v0_id, v1_id, v2_id});
    auto u1 = find_other(tet_attrs[t1].conn, {v0_id, v1_id, v2_id});

    //
    auto new_tets = std::vector<Vec4i>{tet_attrs[t0].conn, tet_attrs[t0].conn, tet_attrs[t0].conn};
    replace(new_tets[0], v0_id, u1);
    replace(new_tets[1], v1_id, u1);
    replace(new_tets[2], v2_id, u1);

    for (auto t : new_tets) {
        if (!tetra_validity(vert_attrs, t)) return false;
    }

    auto after_quality = compute_quality(vert_attrs, new_tets);
    if (before_quality < after_quality) return false;

    auto after_size = max_tetra_sizes(vert_attrs, new_tets);
    if (after_size > size_control) return false;

    update_tetra_conn(vert_attrs, tet_attrs, vert_conn, affected, new_tets, {}, {});
    if (!tetmesh_sanity(tet_attrs, vert_attrs, vert_conn, pc))
        throw std::runtime_error("Sanity Error.");
    return true;
}


void compact_tetmesh(
    std::vector<prism::tet::VertAttr>& vert_info,
    std::vector<prism::tet::TetAttr>& tet_info,
    std::vector<std::vector<int>>& vert_tet_conn,
    PrismCage* pc)
{
    //
    auto vert_map_old2new = std::vector<int>(vert_info.size(), -1);
    auto real_vnum = 0, real_tnum = 0;
    for (auto i = 0; i < vert_info.size(); i++) {
        if (vert_tet_conn[i].empty()) { // remove isolated
            continue;
        }
        vert_map_old2new[i] = real_vnum;
        if (real_vnum != i) vert_info[real_vnum] = std::move(vert_info[i]);
        real_vnum++;
    }
    vert_info.resize(real_vnum);
    auto new_vt_conn = std::vector<std::vector<int>>(real_vnum);
    auto tet_map_old2new = std::vector<int>(tet_info.size(), -1);
    for (auto i = 0; i < tet_info.size(); i++) {
        if (tet_info[i].is_removed) continue;
        tet_map_old2new[i] = real_tnum;
        if (real_tnum != i) tet_info[real_tnum] = std::move(tet_info[i]);
        for (auto j = 0; j < 4; j++) {
            auto& vi = tet_info[real_tnum].conn[j];
            vi = vert_map_old2new[vi];
            assert(vi != -1);
            new_vt_conn[vi].push_back(real_tnum);
        }
        real_tnum++;
    }
    tet_info.resize(real_tnum);

    vert_tet_conn = std::move(new_vt_conn);
    if (pc != nullptr) {
        Eigen::VectorXi vid_ind, vid_map;
        std::vector<int> face_map_o2n;
        pc->cleanup_empty_faces(vid_map, vid_ind, face_map_o2n);

        for (auto& v : vert_info) {
            if (v.mid_id != -1) v.mid_id = vid_map[v.mid_id];
        }
        for (auto& t : tet_info) {
            auto& p = t.prism_id;
            for (auto j = 0; j < 4; j++) {
                if (p[j] != -1) {
                    p[j] = face_map_o2n[p[j]];
                    assert(p[j] != -1);
                }
            }
        }
    }
}

} // namespace prism::tet