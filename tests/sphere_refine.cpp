#include <doctest.h>
#include <geogram/basic/geometry.h>
#include <igl/Timer.h>
#include <igl/avg_edge_length.h>
#include <igl/combine.h>
#include <igl/read_triangle_mesh.h>
#include <spdlog/fmt/bundled/ranges.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <batm/tetra_utils.hpp>
#include <highfive/H5Easy.hpp>
#include <iterator>
#include <numeric>
#include <prism/common.hpp>
#include <prism/geogram/AABB.hpp>
#include <prism/geogram/geogram_utils.hpp>
#include <prism/local_operations/remesh_pass.hpp>
#include <prism/spatial-hash/AABB_hash.hpp>
#include <prism/spatial-hash/self_intersection.hpp>
#include <queue>
#include <tuple>
#include "prism/PrismCage.hpp"
#include "spdlog/common.h"

auto prepare = [](auto& pc) {
    std::string filename = "../tests/data/sphere_40.obj.h5";
    H5Easy::File file(filename, H5Easy::File::ReadOnly);
    auto tet_v = H5Easy::load<RowMatd>(file, "tet_v");
    auto tet_t = H5Easy::load<RowMati>(file, "tet_t");
    Eigen::VectorXi tet_v_pid = -Eigen::VectorXi::Ones(tet_v.rows());
    for (auto i = 0; i < pc.mid.size(); i++) tet_v_pid[i] = i;
    spdlog::info("Loading v {}, t {} ", tet_v.rows(), tet_t.rows());
    return prism::tet::prepare_tet_info(pc, tet_v, tet_t, tet_v_pid);
};

auto reload = [](std::string filename, auto& pc) {
    // std::string filename = "../tests/data/sphere_40.obj.h5";
    H5Easy::File file(filename, H5Easy::File::ReadOnly);
    auto tet_v = H5Easy::load<RowMatd>(file, "tet_v");
    auto tet_t = H5Easy::load<RowMati>(file, "tet_t");
    auto tet_v_pid = H5Easy::load<Eigen::VectorXi>(file, "tet_v_pid");
    spdlog::info("Loading v {}, t {} ", tet_v.rows(), tet_t.rows());
    return prism::tet::prepare_tet_info(pc, tet_v, tet_t, tet_v_pid);
};

TEST_CASE("amr-sphere-prepare")
{
    using namespace prism::tet;
    std::string filename = "../tests/data/sphere_40.obj.h5";
    PrismCage pc(filename);
    spdlog::info("Shell size v{}, f{}", pc.base.size(), pc.F.size());
    auto [vert_info, tet_info, vert_tet_conn] = prepare(pc);


    spdlog::set_level(spdlog::level::trace);


    prism::local::RemeshOptions option(pc.mid.size(), 0.1);
    auto check_sorted = [&vert_tet_conn = vert_tet_conn]() -> bool {
        for (auto& arr : vert_tet_conn)
            for (auto i = 0; i < arr.size() - 1; i++)
                if (arr[i] > arr[i + 1]) return false;
        return true;
    };
    assert(check_sorted());
    split_edge(pc, option, vert_info, tet_info, vert_tet_conn, 0, 1);
    spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    assert(check_sorted());
    assert(check_sorted());
    // smooth_vertex(pc, option, vert_info, tet_info, vert_tet_conn, 46);
    // spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
}

// iterate all edges
auto construct_edge_queue = [](const auto& vert_info, const auto& tet_info) {
    auto local_edges =
        std::array<std::array<int, 2>, 6>{{{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
    auto edge_set = std::set<std::tuple<int, int>>();
    for (auto tet : tet_info) {
        if (tet.is_removed) continue;
        for (auto e : local_edges) {
            auto v0 = tet.conn[e[0]], v1 = tet.conn[e[1]];
            if (v0 > v1) continue;
            edge_set.emplace(v0, v1);
        }
    }
    auto edge_queue = std::priority_queue<std::tuple<double, int, int>>();
    for (auto [v0, v1] : edge_set) {
        auto len = (vert_info[v0].pos - vert_info[v1].pos).squaredNorm();
        edge_queue.emplace(len, v0, v1);
    }
    return edge_queue;
};

// iterate all edges
auto construct_face_queue = [](const auto& vert_info, const auto& tet_info) {
    auto local_faces =
        std::array<std::array<int, 3>, 4>{{{0, 1, 2}, {1, 2, 3}, {2, 3, 0}, {3, 0, 1}}};
    auto face_queue = std::priority_queue<std::tuple<double, int, int, int>>();
    auto face_set = std::set<std::array<int, 3>>();
    for (auto tet : tet_info) {
        if (tet.is_removed) continue;
        for (auto e : local_faces) {
            auto tri = std::array<int, 3>{tet.conn[e[0]], tet.conn[e[1]], tet.conn[e[2]]};
            std::sort(tri.begin(), tri.end());
            face_set.insert(tri);
        }
    }
    for (auto f : face_set) {
        face_queue.emplace(0, f[0], f[1], f[2]);
    }
    return face_queue;
};


TEST_CASE("sphere-tet-swap")
{
    std::string filename = "../tests/data/sphere_40.obj.h5";
    PrismCage pc(filename);
    auto [vert_info, tet_info, vert_tet_conn] = prepare(pc);
    prism::local::RemeshOptions option(pc.mid.size(), 0.1);


    auto edge_queue = construct_edge_queue(vert_info, tet_info);
    auto face_queue = construct_face_queue(vert_info, tet_info);
    spdlog::info("edge queue size {}", edge_queue.size());
    spdlog::info("face queue size {}", face_queue.size());

    // spdlog::set_level(spdlog::level::trace);
    spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    while (!face_queue.empty()) {
        auto [len, v0, v1, v2] = face_queue.top();
        face_queue.pop();
        prism::tet::swap_face(pc, option, vert_info, tet_info, vert_tet_conn, v0, v1, v2, 10);
    }
    spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    for (auto t : tet_info) {
        if (!t.is_removed) CHECK(prism::tet::tetra_validity(vert_info, t.conn));
    }
}

auto set_inter = [](auto& A, auto& B) {
    std::vector<int> vec;
    std::set_intersection(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(vec));
    return vec;
};

auto convert_to_VT = [](auto& vert_info, auto& tet_info) {
    RowMatd V(vert_info.size(), 3);
    RowMati T(tet_info.size(), 4);
    Eigen::VectorXi V_pid(V.rows());
    for (auto i = 0; i < vert_info.size(); i++) {
        V.row(i) = vert_info[i].pos;
        V_pid[i] = vert_info[i].mid_id;
    }
    auto actual_tets = 0;
    for (auto& t : tet_info) {
        if (t.is_removed) continue;
        auto& tet = t.conn;
        T.row(actual_tets) << tet[0], tet[1], tet[2], tet[3];
        actual_tets++;
    }
    T.conservativeResize(actual_tets, 4);
    return std::tuple(V, T, V_pid);
};

auto serializer = [](std::string filename, auto& pc, auto& vert_info, auto& tet_info) {
    pc.serialize(
        filename,
        std::function<void(HighFive::File&)>(
            [&vert_info = vert_info, &tet_info = tet_info](HighFive::File& file) {
                RowMatd V;
                Eigen::VectorXi V_pid;
                RowMati T;
                std::tie(V, T, V_pid) = convert_to_VT(vert_info, tet_info);
                Eigen::VectorXd S(T.rows());
                auto Q = S;
                for (auto i = 0; i < T.rows(); i++) {
                    S[i] = prism::tet::circumradi2(
                        V.row(T(i, 0)),
                        V.row(T(i, 1)),
                        V.row(T(i, 2)),
                        V.row(T(i, 3)));
                    Q[i] = prism::tet::tetra_quality(
                        V.row(T(i, 0)),
                        V.row(T(i, 1)),
                        V.row(T(i, 2)),
                        V.row(T(i, 3)));
                }
                spdlog::info("Saving V {} T {}", V.rows(), T.rows());
                H5Easy::dump(file, "tet_v", V);
                H5Easy::dump(file, "tet_t", T);
                H5Easy::dump(file, "tet_v_pid", V_pid);
                H5Easy::dump(file, "tet_size", S);
                H5Easy::dump(file, "tet_qual", Q);
            }));
};

TEST_CASE("split-pass")
{
    std::string filename = "../tests/data/sphere_40.obj.h5";
    PrismCage pc(filename);
    auto [vert_info, tet_info, vert_tet_conn] = prepare(pc);
    prism::local::RemeshOptions option(pc.mid.size(), 0.1);

    // sizing field always 1e-2
    double sizing = 1e-2;
    auto tet_sizeof = [](auto& vert_info, auto& t) {
        return prism::tet::circumradi2(
            vert_info[t[0]].pos,
            vert_info[t[1]].pos,
            vert_info[t[2]].pos,
            vert_info[t[3]].pos);
    };
    auto tet_marker = std::vector<bool>(tet_info.size(), false);
    for (auto i = 0; i < tet_info.size(); i++) {
        auto r = tet_sizeof(vert_info, tet_info[i].conn);
        if (r > sizing) tet_marker[i] = true;
    }

    auto local_edges =
        std::array<std::array<int, 2>, 6>{{{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
    auto construct_split_queue =
        [&local_edges](const auto& vert_info, const auto& tet_info, const auto& marker) {
            assert(marker.size() == tet_info.size());
            auto edge_set = std::set<std::tuple<int, int>>();
            for (auto i = 0; i < marker.size(); i++) {
                if (marker[i] == false) continue;
                auto& tet = tet_info[i];
                for (auto e : local_edges) {
                    auto v0 = tet.conn[e[0]], v1 = tet.conn[e[1]];
                    if (v0 > v1) continue;
                    edge_set.emplace(v0, v1);
                }
            }
            auto edge_queue = std::priority_queue<std::tuple<double, int, int>>(); // max queue
            for (auto [v0, v1] : edge_set) {
                auto len = (vert_info[v0].pos - vert_info[v1].pos).squaredNorm();
                edge_queue.emplace(len, v0, v1);
            }
            return edge_queue;
        };

    auto edge_queue = construct_split_queue(vert_info, tet_info, tet_marker);
    REQUIRE_FALSE(edge_queue.empty());
    auto all_distinct_verts = [&vert_info = vert_info]() -> bool {
        std::set<std::array<__int128, 3>> verts;
        for (auto v : vert_info)
            verts.emplace(std::array<__int128, 3>{
                static_cast<__int128>(v.pos[0] * 1e20),
                static_cast<__int128>(v.pos[1] * 1e20),
                static_cast<__int128>(v.pos[2] * 1e20)});
        return verts.size() == vert_info.size();
    };
    auto smallest_pairwise = [&vert_info = vert_info]() -> double {
        auto min_gap = 1e3; // HACK: should be limit, but we are already unit.
        for (auto i = 0; i < vert_info.size(); i++) {
            for (auto j = i + 1; j < vert_info.size(); j++) {
                auto gap = (vert_info[i].pos - vert_info[j].pos).squaredNorm();
                min_gap = std::min(gap, min_gap);
            }
        }
        return min_gap;
    };
    spdlog::enable_backtrace(50);
    auto timestamp = 0;
    // TODO: timestamp.
    // TODO: use tet marker here.
    while (!edge_queue.empty()) {
        auto [len, v0, v1] = edge_queue.top();
        spdlog::trace("Edge Queue {}", edge_queue.size());
        edge_queue.pop();
        // v0,v1 might been outdated?

        auto& nb0 = vert_tet_conn[v0];
        auto& nb1 = vert_tet_conn[v1];
        auto affected = set_inter(nb0, nb1); // removed
        if (affected.empty()) {
            spdlog::trace("outdated edge {} v {}-{}", len, v0, v1);
            continue;
        }

        // spdlog::info("len {}, gap {}", len, smallest_pairwise());
        auto oversize =
            [&affected, &sizing, &tet_sizeof, &vert_info = vert_info, &tet_info = tet_info]() {
                for (auto t : affected) {
                    if (tet_info[t].is_removed) assert(false);
                    auto r = tet_sizeof(vert_info, tet_info[t].conn);
                    if (r > sizing) return true;
                }
                return false;
            }();
        if (!oversize) {
            spdlog::trace("size ok {} v {}-{}", len, v0, v1);
            continue;
        }
        auto old_tet_cnt = tet_info.size();
        auto flag = prism::tet::split_edge(pc, option, vert_info, tet_info, vert_tet_conn, v0, v1);
        if (flag) {
            spdlog::trace("Success len {}, v {}-{}", len, v0, v1);
        } else {
            spdlog::info("Fail {} v {}-{}", len, v0, v1);
        }

        auto new_tet_ids = std::vector<int>{}; // HACK: best to return this info from split edge.
        for (auto i = old_tet_cnt; i < tet_info.size(); i++) {
            new_tet_ids.push_back(i);
        }
        for (auto t : new_tet_ids) {
            assert(tet_info[t].is_removed == false);

            auto r = tet_sizeof(vert_info, tet_info[t].conn);
            if (r > sizing) // add new edges only if tetra is large.
                for (auto e : local_edges) {
                    auto v0 = tet_info[t].conn[e[0]], v1 = tet_info[t].conn[e[1]];
                    auto len = (vert_info[v0].pos - vert_info[v1].pos).squaredNorm();
                    edge_queue.emplace(len, v0, v1);
                }
        }

        // spdlog::info("ts {} gap {}", timestamp, smallest_pairwise());

        if (!all_distinct_verts()) {
            spdlog::dump_backtrace();
            assert(false);
            return;
        }
        timestamp++;
    }

    std::vector<int> remains;
    for (auto i = 0; i < tet_info.size(); i++) {
        if (tet_info[i].is_removed) continue;
        auto r = tet_sizeof(vert_info, tet_info[i].conn);
        if (r > sizing) remains.push_back(i);
    }
    spdlog::info("Remain at large {}", remains);

    serializer("debug0.h5", pc, vert_info, tet_info);
}

TEST_CASE("reload-swap")
{
    std::string filename = "../buildr/debug0.h5";
    PrismCage pc(filename);
    auto [vert_info, tet_info, vert_tet_conn] = reload(filename, pc);

    prism::local::RemeshOptions option(pc.mid.size(), 0.1);
    double sizing = 1e-2;
    if (true) {
        auto face_queue = construct_face_queue(vert_info, tet_info);
        spdlog::info("face queue size {}", face_queue.size());
        spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
        while (!face_queue.empty()) {
            auto [len, v0, v1, v2] = face_queue.top();
            face_queue.pop();
            prism::tet::swap_face(pc, option, vert_info, tet_info, vert_tet_conn, v0, v1, v2, sizing);
        }
        spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    }
    if (true) {
        auto edge_queue = construct_edge_queue(vert_info, tet_info);
        spdlog::info("edge queue size {}", edge_queue.size());
        while (!edge_queue.empty()) {
            auto [len, v0, v1] = edge_queue.top();
            edge_queue.pop();
            prism::tet::swap_edge(pc, option, vert_info, tet_info, vert_tet_conn, v0, v1, sizing);
        }
        spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    }

    auto construct_collapse_queue = [](const auto& vert_info, const auto& tet_info) {
        auto local_edges =
            std::array<std::array<int, 2>, 6>{{{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
        auto edge_set = std::set<std::tuple<int, int>>();
        for (auto tet : tet_info) {
            if (tet.is_removed) continue;
            for (auto e : local_edges) {
                auto v0 = tet.conn[e[0]], v1 = tet.conn[e[1]];
                if (v0 > v1) continue;
                edge_set.emplace(v0, v1);
            }
        }
        auto edge_queue = std::priority_queue<std::tuple<double, int, int>>(); // max queue, but we want smaller first.
        for (auto [v0, v1] : edge_set) {
            auto len = (vert_info[v0].pos - vert_info[v1].pos).squaredNorm();
            edge_queue.emplace(-len, v0, v1);
        }
        return edge_queue;
    };

    {
        auto edge_queue = construct_collapse_queue(vert_info, tet_info);
        spdlog::info("edge queue size {}", edge_queue.size());
        while (!edge_queue.empty()) {
            auto [len, v0, v1] = edge_queue.top();
            edge_queue.pop();
            {
                auto& nb1 = vert_tet_conn[v0];
                auto& nb2 = vert_tet_conn[v1];
                auto affected = set_inter(nb1, nb2);
                if (affected.empty()) continue;
            }
            prism::tet::collapse_edge(pc, option, vert_info, tet_info, vert_tet_conn, v0, v1, sizing);
        }
        spdlog::info("Size {} {}", vert_info.size(), tet_info.size());
    }
    for (auto t : tet_info) {
        if (!t.is_removed) CHECK(prism::tet::tetra_validity(vert_info, t.conn));
    }
    serializer("../buildr/debug1.h5", pc, vert_info, tet_info);
}