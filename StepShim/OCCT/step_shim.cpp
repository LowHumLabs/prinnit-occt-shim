// step_shim.cpp — the C ABI declared in ../include/step_shim.h, over OCCT.
//
// scripts/package-occt.sh links this file with OCCT into PrinnitOCCT.framework, which exports only
// the ps_* functions. OCCT and this shim ship as one dynamic framework, and this file's source is
// published under the LGPL-2.1, so the framework can be rebuilt with a changed OCCT.
//
// Every entry point catches C++ exceptions so none can unwind into Swift. OCCT's own checks are
// built in (scripts/build-occt.sh keeps exceptions on in release), so damaged input raises
// Standard_Failure here instead of reading out of bounds.

#include "step_shim.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <NCollection_Map.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_DocumentExplorer.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

namespace {

struct Part {
    PSPartKind kind = PSPartKindSolid;
    std::string name;
    bool hasColor = false;
    float color[3] = {0, 0, 0};
    std::vector<double> positions;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> triangleFaces;
    size_t unmeshedFaces = 0;
};

}  // namespace

struct PSModel {
    PSStatus status = PSStatusOK;
    std::string message;
    std::string fileUnit;
    std::vector<PSSurface> faces;
    std::vector<Part> parts;
};

namespace {

/// Lets OCCT's long operations (transfer, meshing) observe the caller's cancel flag.
class CancelIndicator : public Message_ProgressIndicator {
public:
    explicit CancelIndicator(const volatile bool *flag) : flag_(flag) {}
    bool UserBreak() override { return flag_ != nullptr && *flag_; }
    void Show(const Message_ProgressScope &, const bool) override {}

private:
    const volatile bool *flag_;
};

void SetVector(double out[3], const gp_XYZ &v) {
    out[0] = v.X();
    out[1] = v.Y();
    out[2] = v.Z();
}

/// The face's exact surface in world coordinates. `flip` is true when the face's material side is
/// opposite to its surface's natural normal (a reversed face, or a mirroring placement).
PSSurface SurfaceOf(const TopoDS_Face &face, bool flip) {
    PSSurface out;
    std::memset(&out, 0, sizeof(out));
    out.kind = PSSurfaceFreeform;
    // The adaptor applies the face's location, so the geometry below is already placed.
    BRepAdaptor_Surface surface(face, false);
    switch (surface.GetType()) {
        case GeomAbs_Plane: {
            const gp_Pln plane = surface.Plane();
            const gp_Ax3 &frame = plane.Position();
            // The natural normal is X × Y (gp_Ax3 may be left-handed after a mirror).
            gp_Dir normal = frame.XDirection().Crossed(frame.YDirection());
            if (flip) normal.Reverse();
            out.kind = PSSurfacePlane;
            SetVector(out.origin, frame.Location().XYZ());
            SetVector(out.axis, normal.XYZ());
            break;
        }
        case GeomAbs_Cylinder: {
            const gp_Cylinder cylinder = surface.Cylinder();
            out.kind = PSSurfaceCylinder;
            SetVector(out.origin, cylinder.Location().XYZ());
            SetVector(out.axis, cylinder.Axis().Direction().XYZ());
            out.radius = cylinder.Radius();
            break;
        }
        case GeomAbs_Cone: {
            const gp_Cone cone = surface.Cone();
            const double semiAngle = cone.SemiAngle();
            // A negative semi-angle narrows along the axis: the opening is the other way.
            gp_Dir axis = cone.Axis().Direction();
            if (semiAngle < 0) axis.Reverse();
            out.kind = PSSurfaceCone;
            SetVector(out.origin, cone.Apex().XYZ());
            SetVector(out.axis, axis.XYZ());
            out.angle = std::fabs(semiAngle);
            break;
        }
        case GeomAbs_Sphere: {
            const gp_Sphere sphere = surface.Sphere();
            out.kind = PSSurfaceSphere;
            SetVector(out.origin, sphere.Location().XYZ());
            out.radius = sphere.Radius();
            break;
        }
        case GeomAbs_Torus: {
            const gp_Torus torus = surface.Torus();
            out.kind = PSSurfaceTorus;
            SetVector(out.origin, torus.Location().XYZ());
            SetVector(out.axis, torus.Axis().Direction().XYZ());
            out.radius = torus.MajorRadius();
            out.radius2 = torus.MinorRadius();
            break;
        }
        default:
            break;
    }
    return out;
}

/// Shares vertices that lie within `tolerance` of each other. The mesher discretizes an edge once
/// for both faces beside it, but a seam (where a round face meets itself) is evaluated at both ends
/// of its parameter range, so its two copies can differ in the last bits.
class Welder {
public:
    explicit Welder(std::vector<double> &positions, double tolerance = 1e-6)
        : positions_(positions), tolerance_(tolerance) {}

    uint32_t Index(const gp_Pnt &p) {
        const int64_t cx = Cell(p.X()), cy = Cell(p.Y()), cz = Cell(p.Z());
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    auto found = cells_.find(Key{cx + dx, cy + dy, cz + dz});
                    if (found == cells_.end()) continue;
                    for (uint32_t v : found->second) {
                        const double ex = positions_[3 * v] - p.X();
                        const double ey = positions_[3 * v + 1] - p.Y();
                        const double ez = positions_[3 * v + 2] - p.Z();
                        if (ex * ex + ey * ey + ez * ez <= tolerance_ * tolerance_) return v;
                    }
                }
            }
        }
        const uint32_t v = static_cast<uint32_t>(positions_.size() / 3);
        positions_.insert(positions_.end(), {p.X(), p.Y(), p.Z()});
        cells_[Key{cx, cy, cz}].push_back(v);
        return v;
    }

private:
    struct Key {
        int64_t x, y, z;
        bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            return std::hash<int64_t>()(k.x) ^ (std::hash<int64_t>()(k.y) * 31) ^ (std::hash<int64_t>()(k.z) * 1009);
        }
    };

    int64_t Cell(double v) const { return static_cast<int64_t>(std::floor(v / tolerance_)); }

    std::vector<double> &positions_;
    double tolerance_;
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> cells_;
};

/// Appends the triangles of `shape`'s faces (already meshed) to `part`, registering each face in
/// the model's face table. Vertices within a micrometre are shared, which joins faces along the
/// edges the mesher discretized once for both; triangles that welding collapses are dropped.
void AppendTriangles(PSModel &model, Part &part, const TopoDS_Shape &shape) {
    Welder welder(part.positions);
    NCollection_Map<TopoDS_Shape, TopTools_ShapeMapHasher> seen;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face &face = TopoDS::Face(explorer.Current());
        if (!seen.Add(face)) continue;
        TopLoc_Location location;
        const occ::handle<Poly_Triangulation> &mesh = BRep_Tool::Triangulation(face, location);
        if (mesh.IsNull() || mesh->NbTriangles() == 0) {
            part.unmeshedFaces += 1;
            continue;
        }
        const gp_Trsf transform = location.Transformation();
        const bool mirrored = transform.VectorialPart().Determinant() < 0;
        const bool flip = (face.Orientation() == TopAbs_REVERSED) != mirrored;

        const uint32_t faceIndex = static_cast<uint32_t>(model.faces.size());
        model.faces.push_back(SurfaceOf(face, flip));

        std::vector<uint32_t> index(static_cast<size_t>(mesh->NbNodes()) + 1);
        for (int n = 1; n <= mesh->NbNodes(); ++n) {
            index[n] = welder.Index(mesh->Node(n).Transformed(transform));
        }
        for (int t = 1; t <= mesh->NbTriangles(); ++t) {
            int a, b, c;
            mesh->Triangle(t).Get(a, b, c);
            if (flip) std::swap(b, c);
            if (index[a] == index[b] || index[b] == index[c] || index[a] == index[c]) continue;
            part.triangles.insert(part.triangles.end(), {index[a], index[b], index[c]});
            part.triangleFaces.push_back(faceIndex);
        }
    }
}

std::string NameOf(const TDF_Label &label) {
    occ::handle<TDataStd_Name> name;
    if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), name)) return {};
    return TCollection_AsciiString(name->Get()).ToCString();
}

/// Loose faces sewn together: closed shells become solids, the rest stay shells.
void SewFaces(const TopoDS_Shape &faces, std::vector<std::pair<PSPartKind, TopoDS_Shape>> &out) {
    BRepBuilderAPI_Sewing sewing(1e-3);
    for (TopExp_Explorer explorer(faces, TopAbs_FACE); explorer.More(); explorer.Next()) {
        sewing.Add(explorer.Current());
    }
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    bool any = false;
    for (TopExp_Explorer explorer(sewn, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        const TopoDS_Shell &shell = TopoDS::Shell(explorer.Current());
        any = true;
        if (BRep_Tool::IsClosed(shell)) {
            ShapeFix_Solid fix;
            const TopoDS_Solid solid = fix.SolidFromShell(shell);
            if (!solid.IsNull()) {
                out.emplace_back(PSPartKindSolid, solid);
                continue;
            }
        }
        out.emplace_back(PSPartKindShell, shell);
    }
    if (!any) {
        out.emplace_back(PSPartKindFaces, sewn);
    }
}

void Read(PSModel &model, const uint8_t *data, size_t length, double linearDeflection,
          double angularDeflection, const volatile bool *cancel) {
    occ::handle<CancelIndicator> indicator = new CancelIndicator(cancel);
    Message_ProgressScope scope(indicator->Start(), "STEP", 3);

    STEPCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    reader.SetLayerMode(false);
    reader.SetPropsMode(false);
    reader.SetGDTMode(false);
    reader.SetMatMode(false);
    reader.SetViewMode(false);
    reader.SetSHUOMode(false);
    reader.SetMetaMode(false);

    std::istringstream stream(std::string(reinterpret_cast<const char *>(data), length));
    if (reader.ReadStream("model.step", stream) != IFSelect_RetDone) {
        model.status = PSStatusReadFailed;
        model.message = "This isn't a STEP file Prinnit can read";
        return;
    }
    NCollection_Sequence<TCollection_AsciiString> lengthUnits, angleUnits, solidAngleUnits;
    reader.ChangeReader().FileUnits(lengthUnits, angleUnits, solidAngleUnits);
    if (!lengthUnits.IsEmpty()) model.fileUnit = lengthUnits.First().ToCString();

    occ::handle<TDocStd_Document> document = new TDocStd_Document("MDTV-XCAF");
    XCAFDoc_DocumentTool::Set(document->Main(), true);
    // Millimetres: the reader converts the file's unit to the document's.
    XCAFDoc_DocumentTool::SetLengthUnit(document, 1.0, UnitsMethods_LengthUnit_Millimeter);
    if (!reader.Transfer(document, scope.Next())) {
        model.status = scope.UserBreak() ? PSStatusCancelled : PSStatusReadFailed;
        model.message = scope.UserBreak() ? "Cancelled" : "The STEP file has nothing Prinnit can use";
        return;
    }
    if (scope.UserBreak()) {
        model.status = PSStatusCancelled;
        return;
    }

    // Every leaf product as placed in the assembly, with its inherited colour.
    struct Placed {
        std::string name;
        bool hasColor;
        float color[3];
        PSPartKind kind;
        TopoDS_Shape shape;
    };
    std::vector<Placed> placed;
    for (XCAFPrs_DocumentExplorer explorer(document, XCAFPrs_DocumentExplorerFlags_OnlyLeafNodes);
         explorer.More(); explorer.Next()) {
        const XCAFPrs_DocumentNode &node = explorer.Current();
        TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(node.RefLabel);
        if (shape.IsNull()) continue;
        shape.Location(node.Location, false);
        std::string name = NameOf(node.RefLabel);
        if (name.empty()) name = NameOf(node.Label);
        float color[3] = {0, 0, 0};
        const bool hasColor = node.Style.IsSetColorSurf();
        if (hasColor) {
            const Quantity_Color &c = node.Style.GetColorSurf();
            // sRGB, as the app stores colours.
            NCollection_Vec3<float> srgb = Quantity_Color::Convert_LinearRGB_To_sRGB(c.Rgb());
            color[0] = srgb.r();
            color[1] = srgb.g();
            color[2] = srgb.b();
        }
        std::vector<std::pair<PSPartKind, TopoDS_Shape>> pieces;
        NCollection_Map<TopoDS_Shape, TopTools_ShapeMapHasher> inSolid;
        for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next()) {
            pieces.emplace_back(PSPartKindSolid, solids.Current());
            for (TopExp_Explorer faces(solids.Current(), TopAbs_FACE); faces.More(); faces.Next()) {
                inSolid.Add(faces.Current());
            }
        }
        // Faces outside every solid: a surface model. Sewing may close it into a solid.
        TopoDS_Compound loose;
        BRep_Builder builder;
        builder.MakeCompound(loose);
        bool anyLoose = false;
        for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
            if (inSolid.Contains(faces.Current())) continue;
            builder.Add(loose, faces.Current());
            anyLoose = true;
        }
        if (anyLoose) SewFaces(loose, pieces);
        for (auto &piece : pieces) {
            Placed p{name, hasColor, {color[0], color[1], color[2]}, piece.first, piece.second};
            placed.push_back(std::move(p));
        }
    }
    if (placed.empty()) {
        model.status = PSStatusNoShapes;
        model.message = "The STEP file has no solids or surfaces";
        return;
    }

    IMeshTools_Parameters parameters;
    parameters.Deflection = linearDeflection;
    parameters.Angle = angularDeflection;
    parameters.Relative = false;
    parameters.InParallel = true;
    Message_ProgressScope meshing(scope.Next(), "Mesh", static_cast<double>(placed.size()));
    for (const Placed &p : placed) {
        BRepMesh_IncrementalMesh mesher(p.shape, parameters, meshing.Next());
        if (meshing.UserBreak()) {
            model.status = PSStatusCancelled;
            return;
        }
        Part part;
        part.kind = p.kind;
        part.name = p.name;
        part.hasColor = p.hasColor;
        std::memcpy(part.color, p.color, sizeof(part.color));
        AppendTriangles(model, part, p.shape);
        if (!part.triangles.empty()) model.parts.push_back(std::move(part));
    }
    if (model.parts.empty()) {
        model.status = PSStatusNoShapes;
        model.message = "None of the STEP file's shapes could be meshed";
    }
}

}  // namespace

namespace {

const Part *PartAt(const PSModel *model, size_t part) {
    if (model == nullptr || part >= model->parts.size()) return nullptr;
    return &model->parts[part];
}

}  // namespace

extern "C" {

PSModel *ps_step_read(const uint8_t *data, size_t length, double linear_deflection,
                      double angular_deflection, const volatile bool *cancel) {
    // OCCT's STEP translator keeps global state (its schema and parameter tables), so reads take
    // turns. Meshing inside a read still runs in parallel.
    static std::mutex readLock;
    std::lock_guard<std::mutex> lock(readLock);
    PSModel *model = new PSModel();
    try {
        Read(*model, data, length, linear_deflection, angular_deflection, cancel);
    } catch (const Standard_Failure &failure) {
        model->status = PSStatusException;
        model->message = std::string(failure.ExceptionType()) + ": " + failure.what();
        model->parts.clear();
    } catch (const std::exception &error) {
        model->status = PSStatusException;
        model->message = error.what();
        model->parts.clear();
    } catch (...) {
        model->status = PSStatusException;
        model->message = "an unknown error";
        model->parts.clear();
    }
    return model;
}

void ps_step_release(PSModel *model) { delete model; }

PSStatus ps_step_status(const PSModel *model) { return model ? model->status : PSStatusException; }

const char *ps_step_message(const PSModel *model) { return model ? model->message.c_str() : ""; }

const char *ps_step_file_unit(const PSModel *model) { return model ? model->fileUnit.c_str() : ""; }

size_t ps_step_face_count(const PSModel *model) { return model ? model->faces.size() : 0; }

void ps_step_face_surface(const PSModel *model, size_t index, PSSurface *out) {
    if (out == nullptr) return;
    if (model == nullptr || index >= model->faces.size()) {
        std::memset(out, 0, sizeof(*out));
        out->kind = PSSurfaceFreeform;
        return;
    }
    *out = model->faces[index];
}

size_t ps_step_part_count(const PSModel *model) { return model ? model->parts.size() : 0; }

PSPartKind ps_step_part_kind(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->kind : PSPartKindFaces;
}

const char *ps_step_part_name(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->name.c_str() : "";
}

bool ps_step_part_color(const PSModel *model, size_t part, float *rgb) {
    const Part *p = PartAt(model, part);
    if (p == nullptr || !p->hasColor || rgb == nullptr) return false;
    std::memcpy(rgb, p->color, sizeof(p->color));
    return true;
}

size_t ps_step_part_vertex_count(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->positions.size() / 3 : 0;
}

const double *ps_step_part_positions(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->positions.data() : nullptr;
}

size_t ps_step_part_triangle_count(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->triangleFaces.size() : 0;
}

const uint32_t *ps_step_part_triangles(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->triangles.data() : nullptr;
}

const uint32_t *ps_step_part_triangle_faces(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->triangleFaces.data() : nullptr;
}

size_t ps_step_part_unmeshed_faces(const PSModel *model, size_t part) {
    const Part *p = PartAt(model, part);
    return p ? p->unmeshedFaces : 0;
}

const char *ps_occt_version(void) { return OCC_VERSION_COMPLETE; }

bool ps_occt_available(void) { return true; }

}  // extern "C"
