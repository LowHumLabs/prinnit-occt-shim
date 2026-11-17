// step_shim.h — Prinnit's thin C ABI over Open CASCADE Technology's STEP reader and mesher.
//
// Swift imports this header as a plain C module, so no Swift module has to enable C++ interop.
// All C++ lives in step_shim.cpp. OCCT is LGPL-2.1 with the Open CASCADE exception; this shim's
// source is published under the LGPL-2.1.
//
// One call reads a STEP file and returns its parts already meshed: every solid (and every shell or
// face that isn't part of a solid) in world coordinates and millimetres, each triangle carrying the
// index of the B-rep face it came from, and each face its exact surface where it has one.
//
// Ownership: ps_step_read returns a new model the caller releases with ps_step_release. Pointers
// returned by the accessors stay valid until then. A model is immutable.

#ifndef PRINNIT_STEP_SHIM_H
#define PRINNIT_STEP_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PSModel PSModel;

typedef enum PSStatus {
    PSStatusOK = 0,
    /// The STEP reader rejected the file (not STEP, or damaged).
    PSStatusReadFailed = 1,
    /// The file was read but holds no shapes.
    PSStatusNoShapes = 2,
    /// OCCT raised an exception; ps_step_message says what.
    PSStatusException = 3,
    PSStatusCancelled = 4,
} PSStatus;

/// What a part is in the file.
typedef enum PSPartKind {
    /// A solid: closed unless the file's geometry has gaps.
    PSPartKindSolid = 0,
    /// A shell that isn't part of a solid (a surface model); usually open.
    PSPartKindShell = 1,
    /// Loose faces that belong to no shell, gathered per product.
    PSPartKindFaces = 2,
} PSPartKind;

/// The exact surface of a B-rep face (`AnalyticSurface` on the Swift side). Points and directions
/// are in world millimetres. Directions are unit length.
typedef enum PSSurfaceKind {
    /// `axis` is the outward normal (the face's orientation applied), `origin` a point on it.
    PSSurfacePlane = 0,
    /// `origin` on the axis, `axis` its direction, `radius`.
    PSSurfaceCylinder = 1,
    /// `origin` is the apex, `axis` points from the apex into the cone's opening, `angle` is the
    /// half-angle in radians (0 < angle < π/2).
    PSSurfaceCone = 2,
    /// `origin` is the centre, `radius`.
    PSSurfaceSphere = 3,
    /// `origin` is the centre, `axis` its axis, `radius` the major and `radius2` the minor radius.
    PSSurfaceTorus = 4,
    /// B-spline, Bézier, offset, swept and other surfaces: no analytic description.
    PSSurfaceFreeform = 5,
} PSSurfaceKind;

typedef struct PSSurface {
    PSSurfaceKind kind;
    double origin[3];
    double axis[3];
    double radius;
    double radius2;
    double angle;
} PSSurface;

/// Reads `length` bytes of a STEP file (AP203, AP214 or AP242) and meshes it.
///
/// - linear_deflection: the largest distance (mm) between a triangle and its surface; Prinnit uses
///   its print chord tolerance.
/// - angular_deflection: the largest angle (radians) between neighbouring triangles on a curved
///   face.
/// - cancel: optional; polled while reading and meshing. Set `*cancel` to true from any thread to
///   stop; the model then has PSStatusCancelled.
///
/// Safe to call from any thread; concurrent reads take turns (OCCT's STEP translator keeps global
/// state). Never returns NULL: check ps_step_status.
PSModel *ps_step_read(const uint8_t *data, size_t length, double linear_deflection,
                      double angular_deflection, const volatile bool *cancel);

void ps_step_release(PSModel *model);

PSStatus ps_step_status(const PSModel *model);
/// What went wrong, or an empty string.
const char *ps_step_message(const PSModel *model);
/// The length unit the file was written in ("millimetre", "inch", …), or an empty string. Shapes
/// are already converted to millimetres.
const char *ps_step_file_unit(const PSModel *model);

/// Faces across all parts; triangle face indices index this table.
size_t ps_step_face_count(const PSModel *model);
/// Writes face `index`'s surface to `out`.
void ps_step_face_surface(const PSModel *model, size_t index, PSSurface *out);

size_t ps_step_part_count(const PSModel *model);
PSPartKind ps_step_part_kind(const PSModel *model, size_t part);
/// The product's name in the file (UTF-8), or an empty string.
const char *ps_step_part_name(const PSModel *model, size_t part);
/// The part's surface colour, if the file gives one: writes r, g, b in 0…1 to `rgb`.
bool ps_step_part_color(const PSModel *model, size_t part, float *rgb);
size_t ps_step_part_vertex_count(const PSModel *model, size_t part);
/// `vertex_count * 3` doubles, x, y, z per vertex (mm). Vertices on a shared edge are shared.
const double *ps_step_part_positions(const PSModel *model, size_t part);
size_t ps_step_part_triangle_count(const PSModel *model, size_t part);
/// `triangle_count * 3` vertex indices, counter-clockwise seen from outside the material.
const uint32_t *ps_step_part_triangles(const PSModel *model, size_t part);
/// `triangle_count` indices into the face table.
const uint32_t *ps_step_part_triangle_faces(const PSModel *model, size_t part);
/// Faces of the part that the mesher couldn't triangulate (the part then has a hole).
size_t ps_step_part_unmeshed_faces(const PSModel *model, size_t part);

/// The OCCT version the shim was built against, e.g. "8.0.1", or an empty string.
const char *ps_occt_version(void);

/// Whether this build has OCCT. A checkout that hasn't run scripts/build-occt.sh builds the shim
/// without it: every read then fails with PSStatusReadFailed and a message saying so.
bool ps_occt_available(void);

#ifdef __cplusplus
}
#endif

#endif // PRINNIT_STEP_SHIM_H
