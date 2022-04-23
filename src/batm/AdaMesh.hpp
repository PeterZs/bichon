#pragma once
#include <wmtk/ConcurrentTetMesh.h>
#include <wmtk/TetMesh.h>

#include <Rational.h>
#include <Eigen/Core>
#include <stdexcept>
#include "prism/common.hpp"

namespace wmtk::prism {

using Vector3r = Eigen::Matrix<apps::Rational, 3, 1>;
using Vector2r = Eigen::Matrix<apps::Rational, 2, 1>;
struct AdaMesh : public wmtk::TetMesh
{
    struct VertexAttributes
    {
        Vec3d m_posf;
        Vector3r m_posr = Vector3r::Zero();
        bool rounded = false;
        bool is_removed = false;
        bool m_is_on_surface = false;
        std::vector<int> on_bbox_faces;
        VertexAttributes(){};
        VertexAttributes(const Vec3d& v)
            : m_posf(v)
            , rounded(true)
        {
            for (auto i = 0; i < 3; i++) m_posr[i] = apps::Rational(v[i]);
        };
        VertexAttributes(const Vector3r& v)
            : m_posr(v)
            , rounded(false)
        {}
    };
    struct TetAttributes
    {
        std::array<std::set<int>, 4> track_prisms;
        bool is_removed = false;
    };
    struct FaceAttributes
    {
        std::set<int> track_prisms;
        bool m_is_surface_fs = false;
        int m_is_bbox_fs = -1;
        int m_surface_tags = -1;
        void reset()
        {
            m_is_surface_fs = false;
            m_is_bbox_fs = -1;
            m_surface_tags = -1;
        }
    };
    using VertAttCol = wmtk::AttributeCollection<VertexAttributes>;
    VertAttCol vertex_attrs;
    using TetAttCol = wmtk::AttributeCollection<TetAttributes>;
    TetAttCol tet_attrs;
    using FaceAttCol = wmtk::AttributeCollection<FaceAttributes>;
    FaceAttCol m_face_attribute;


public:
    AdaMesh(const RowMatd& V, const RowMati& T); // initialize topology and attrs
    void insert_all_points(
        const std::vector<Vec3d>& points,
        const std::vector<int>& hint_tid); // insert points
    void insert_all_triangles(const std::vector<std::array<size_t, 3>>& tris);

public: // callbacks
    struct TriangleInsertionLocalInfoCache
    {
        // local info: for each face insertion
        int face_id;
        std::vector<std::array<size_t, 3>> old_face_vids;
    };
    TriangleInsertionLocalInfoCache triangle_insertion_local_cache;
    std::map<std::array<size_t, 3>, std::vector<int>> tet_face_tags;
    bool triangle_insertion_before(const std::vector<Tuple>& tup) override;
    bool triangle_insertion_after(const std::vector<std::vector<Tuple>>& new_faces) override;
    void finalize_triangle_insertion(const std::vector<std::array<size_t, 3>>& tris);

public:
    bool round(size_t vid);
    bool is_invert(const Tuple& t) const;
    double quality(const Tuple& t) const;

    std::map<std::array<size_t, 3>, FaceAttributes> cache_changed_faces;
    void split_all_edges();
    bool split_edge_before(const Tuple& t) override;
    bool split_edge_after(const Tuple& loc) override;

    void smooth_all_vertices();
    bool smooth_before(const Tuple& t) override;
    bool smooth_after(const Tuple& t) override;

    void collapse_all_edges(bool is_limit_length = true);
    bool collapse_edge_before(const Tuple& t) override;
    bool collapse_edge_after(const Tuple& t) override;

    void swap_all_edges_44();
    bool swap_edge_44_before(const Tuple& t) override;
    bool swap_edge_44_after(const Tuple& t) override;

    void swap_all_edges();
    bool swap_edge_before(const Tuple& t) override;
    bool swap_edge_after(const Tuple& t) override;

    void swap_all_faces();
    bool swap_face_before(const Tuple& t) override;
    bool swap_face_after(const Tuple& t) override;
};
} // namespace wmtk::prism