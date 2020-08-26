/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "xxhash.h"

/* History
    the images undo history is stored in a linked list.  Every time we call
    image_history_push, we add the current image snapshot in the list.

    For example, if we did three operations, A, B, C, and now the image is
    in the D state, the history list looks like this:

    img->history                                        img
        |                                                |
        v                                                v
    +--------+       +--------+       +--------+      +--------+
    |        |       |        |       |        |      |        |
    |   A    |------>|   B    |------>|   C    |----->|   D    |
    |        |       |        |       |        |      |        |
    +--------+       +--------+       +--------+      +--------+

    After an undo, we get:

    img->history                        img
        |                                |
        v                                v
    +--------+       +--------+       +--------+     +--------+
    |        |       |        |       |        |     |        |
    |   A    |------>|   B    |------>|   C    |---->|   D    |
    |        |       |        |       |        |     |        |
    +--------+       +--------+       +--------+     +--------+


*/


static bool material_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const material_t *m;
    DL_FOREACH(img->materials, m) {
        if (strcasecmp(m->name, name) == 0) return true;
    }
    return false;
}

static bool layer_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        if (strcasecmp(layer->name, name) == 0) return true;
    }
    return false;
}

static bool camera_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const camera_t *cam;
    DL_FOREACH(img->cameras, cam) {
        if (strcasecmp(cam->name, name) == 0) return true;
    }
    return false;
}

static void make_uniq_name(
        char *buf, int size, const char *base, void *user,
        bool (*name_exists)(void *user, const char *name))
{
    int i = 1, n, len;
    const char *ext;

    // If base if of the form 'abc.<num>' then we turn it into 'abc'
    len = strlen(base);
    ext = strrchr(base, '.');
    if (ext) {
        if (sscanf(ext, ".%d%*c", &n) == 1) {
            len -= strlen(ext);
            i = n;
        }
    }

    for (;; i++) {
        snprintf(buf, size, "%.*s.%d", len, base, i);
        if (!name_exists(user, buf)) break;
    }
}

/*
static layer_t *img_get_layer(const image_t *img, int id)
{
    layer_t *layer;
    if (id == 0) return NULL;
    DL_FOREACH(img->layers, layer)
        if (layer->id == id) return layer;
    assert(false);
    return NULL;
}
*/

static int img_get_new_id(const image_t *img)
{
    int id;
    layer_t *layer;
    for (id = 1;; id++) {
        DL_FOREACH(img->layers, layer)
            if (layer->id == id) break;
        if (layer == NULL) break;
    }
    return id;
}

// Make sure the layer mesh is up to date.
void image_update(image_t *img)
{
    painter_t painter = {};
    uint32_t key;
    layer_t *layer;
    const layer_t *base;

    DL_FOREACH(img->layers, layer) {
        base = layer->parent;
        if (base && layer->base_mesh_key != mesh_get_key(base->mesh)) {
            mesh_set(layer->mesh, base->mesh);
            mesh_move(layer->mesh, layer->mat);
            layer->base_mesh_key = mesh_get_key(base->mesh);
        }
        if (layer->shape) {
            key = XXH32(layer->mat, sizeof(layer->mat), 0);
            key = XXH32(layer->shape, sizeof(layer->shape), key);
            key = XXH32(layer->color, sizeof(layer->color), key);
            if (key != layer->shape_key) {
                painter.mode = MODE_OVER;
                painter.shape = layer->shape;
                painter.box = &goxel.image->box;
                vec4_copy(layer->color, painter.color);
                mesh_clear(layer->mesh);
                mesh_op(layer->mesh, &painter, layer->mat);
                layer->shape_key = key;
            }
        }
    }
}

image_t *image_new(void)
{
    layer_t *layer;
    image_t *img = calloc(1, sizeof(*img));
    const int aabb[2][3] = {{-16, -16, 0}, {16, 16, 32}};
    bbox_from_aabb(img->box, aabb);
    img->export_width = 1024;
    img->export_height = 1024;
    image_add_material(img, NULL);
    image_add_camera(img, NULL);
    layer = image_add_layer(img, NULL);
    layer->visible = true;
    layer->id = img_get_new_id(img);
    layer->material = img->active_material;
    DL_APPEND(img->layers, layer);
    DL_APPEND2(img->history, img, history_prev, history_next);
    img->active_layer = layer;
    // Prevent saving an empty image.
    img->saved_key = image_get_key(img);
    return img;
}

/*
 * Generate a copy of the image that can be put into the history.
 */
static image_t *image_snap(image_t *other)
{
    image_t *img;
    layer_t *layer, *other_layer;
    camera_t *camera, *other_camera;
    material_t *material, *other_material;

    img = calloc(1, sizeof(*img));
    *img = *other;

    img->layers = NULL;
    img->active_layer = NULL;
    DL_FOREACH(other->layers, other_layer) {
        layer = layer_copy(other_layer);
        DL_APPEND(img->layers, layer);
        if (other_layer == other->active_layer)
            img->active_layer = layer;
    }
    assert(img->active_layer);

    img->cameras = NULL;
    img->active_camera = NULL;
    DL_FOREACH(other->cameras, other_camera) {
        camera = camera_copy(other_camera);
        DL_APPEND(img->cameras, camera);
        if (other_camera == other->active_camera)
            img->active_camera = camera;
    }

    img->materials = NULL;
    img->active_material = NULL;
    DL_FOREACH(other->materials, other_material) {
        material = material_copy(other_material);
        DL_APPEND(img->materials, material);
        if (other_material == other->active_material)
            img->active_material = material;
        DL_FOREACH(img->layers, layer) {
            if (layer->material == other_material)
                layer->material = material;
        }
    }

    img->history = img->history_next = img->history_prev = NULL;
    return img;
}


void image_delete(image_t *img)
{
    image_t *hist, *snap, *snap_tmp;
    camera_t *cam;
    layer_t *layer;
    material_t *mat;

    if (!img) return;

    while ((layer = img->layers)) {
        DL_DELETE(img->layers, layer);
        layer_delete(layer);
    }
    while ((cam = img->cameras)) {
        DL_DELETE(img->cameras, cam);
        camera_delete(cam);
    }
    while ((mat = img->materials)) {
        DL_DELETE(img->materials, mat);
        material_delete(mat);
    }

    // Path is shared between images and snaps!
    // XXX: find a better way.
    if (img->history) {
        free(img->path);
        img->path = NULL;
    }

    hist = img->history;
    DL_FOREACH_SAFE2(hist, snap, snap_tmp, history_next) {
        if (snap == img) continue;
        DL_DELETE2(hist, snap, history_prev, history_next);
        image_delete(snap);
    }

    free(img);
}

layer_t *image_add_layer(image_t *img, layer_t *layer)
{
    img = img ?: goxel.image;
    if (!layer)
        layer = layer_new(NULL);

    if (!layer->name[0] || layer_name_exists(img, layer->name)) {
        make_uniq_name(layer->name, sizeof(layer->name), "Layer", img,
                       layer_name_exists);
    }

    layer->visible = true;
    layer->id = img_get_new_id(img);
    layer->material = img->active_material;
    DL_APPEND(img->layers, layer);
    img->active_layer = layer;
    return layer;
}

layer_t *image_add_shape_layer(image_t *img)
{
    layer_t *layer;
    img = img ?: goxel.image;
    layer = layer_new("shape");
    layer->visible = true;
    layer->shape = &shape_sphere;
    vec4_copy(goxel.painter.color, layer->color);
    // If the selection is on use it, otherwise center it in the image.
    if (!box_is_null(goxel.selection)) {
        mat4_copy(goxel.selection, layer->mat);
    } else {
        vec3_copy(img->box[3], layer->mat[3]);
        mat4_iscale(layer->mat, 4, 4, 4);
    }
    layer->id = img_get_new_id(img);
    DL_APPEND(img->layers, layer);
    img->active_layer = layer;
    return layer;
}

void image_delete_layer(image_t *img, layer_t *layer)
{
    layer_t *other;
    img = img ?: goxel.image;
    layer = layer ?: img->active_layer;
    DL_DELETE(img->layers, layer);
    if (layer == img->active_layer) img->active_layer = NULL;

    // Unclone all layers cloned from this one.
    DL_FOREACH(goxel.image->layers, other) {
        if (other->parent == layer) {
            other->parent = NULL;
        }
    }

    layer_delete(layer);
    if (img->layers == NULL) {
        layer = layer_new("unnamed");
        layer->visible = true;
        layer->id = img_get_new_id(img);
        DL_APPEND(img->layers, layer);
    }
    if (!img->active_layer) img->active_layer = img->layers->prev;
}

layer_t *image_duplicate_layer(image_t *img, layer_t *other)
{
    layer_t *layer;
    img = img ?: goxel.image;
    other = other ?: img->active_layer;
    layer = layer_copy(other);
    make_uniq_name(layer->name, sizeof(layer->name), other->name, img,
                   layer_name_exists);
    layer->visible = true;
    layer->id = img_get_new_id(img);
    DL_APPEND(img->layers, layer);
    img->active_layer = layer;
    return layer;
}

void image_merge_visible_layers(image_t *img)
{
    layer_t *layer, *other, *last = NULL;
    img = img ?: goxel.image;
    DL_FOREACH(img->layers, layer) {
        if (!layer->visible) continue;
        layer_unclone(layer);

        if (last) {
            // Unclone all layers cloned from this one.
            DL_FOREACH(goxel.image->layers, other) {
                if (other->parent == last) {
                    other->parent = NULL;
                }
            }
            SWAP(layer->mesh, last->mesh);
            mesh_merge(layer->mesh, last->mesh, MODE_OVER, NULL);
            DL_DELETE(img->layers, last);
            layer_delete(last);
        }
        last = layer;
    }
    if (last) img->active_layer = last;
}


camera_t *image_add_camera(image_t *img, camera_t *cam)
{
    img = img ?: goxel.image;
    if (!cam)
        cam = camera_new(NULL);

    if (!cam->name[0] || camera_name_exists(img, cam->name)) {
        make_uniq_name(cam->name, sizeof(cam->name), "Camera", img,
                       camera_name_exists);
    }
    DL_APPEND(img->cameras, cam);
    img->active_camera = cam;
    return cam;
}

void image_delete_camera(image_t *img, camera_t *cam)
{
    img = img ?: goxel.image;
    cam = cam ?: img->active_camera;
    if (!cam) return;
    DL_DELETE(img->cameras, cam);
    if (cam == img->active_camera)
        img->active_camera = img->cameras;
    camera_delete(cam);
}

void image_move_camera(image_t *img, camera_t *cam, int d)
{
    // XXX: make a generic algo to move objects in a list.
    assert(d == -1 || d == +1);
    camera_t *other = NULL;
    img = img ?: goxel.image;
    cam = cam ?: img->active_camera;
    if (!cam) return;
    if (d == -1) {
        other = cam->next;
        SWAP(other, cam);
    } else if (cam != img->cameras) {
        other = cam->prev;
    }
    if (!other || !cam) return;
    DL_DELETE(img->cameras, cam);
    DL_PREPEND_ELEM(img->cameras, other, cam);
}

static void image_move_camera_up(image_t *img, camera_t *cam)
{
    image_move_camera(img, cam, +1);
}

static void image_move_camera_down(image_t *img, camera_t *cam)
{
    image_move_camera(img, cam, -1);
}

material_t *image_add_material(image_t *img, material_t *mat)
{
    img = img ?: goxel.image;
    if (!mat) {
        mat = material_new(NULL);
        make_uniq_name(mat->name, sizeof(mat->name), "Material", img,
                       material_name_exists);
    }
    assert(!mat->prev);
    DL_APPEND(img->materials, mat);
    img->active_material = mat;
    return mat;
}

void image_delete_material(image_t *img, material_t *mat)
{
    layer_t *layer;

    img = img ?: goxel.image;
    mat = mat ?: img->active_material;
    if (!mat) return;
    DL_DELETE(img->materials, mat);
    if (mat == img->active_material) img->active_material = NULL;
    material_delete(mat);
    DL_FOREACH(img->layers, layer)
        if (layer->material == mat) layer->material = NULL;
}

static void image_auto_resize(image_t *img)
{
    float box[4][4] = {}, layer_box[4][4];
    layer_t *layer;

    img = img ?: goxel.image;
    DL_FOREACH(img->layers, layer) {
        layer_get_bounding_box(layer, layer_box);
        box_union(box, layer_box, box);
    }
    mat4_copy(box, img->box);
}

void image_set(image_t *img, image_t *other)
{
    layer_t *layer, *tmp, *other_layer;
    DL_FOREACH_SAFE(img->layers, layer, tmp) {
        DL_DELETE(img->layers, layer);
        layer_delete(layer);
    }
    DL_FOREACH(other->layers, other_layer) {
        layer = layer_copy(other_layer);
        DL_APPEND(img->layers, layer);
        if (other_layer == other->active_layer)
            img->active_layer = layer;
    }
}

#if 0 // For debugging purpose.
static void debug_print_history(image_t *img)
{
    int i = 0;
    image_t *hist;
    DL_FOREACH2(img->history, hist, history_next) {
        printf("%d%s  ", i++, hist == img ? "*" : " ");
    }
    printf("\n");
}
#else
static void debug_print_history(image_t *img) {}
#endif

void image_history_push(image_t *img)
{
    image_t *snap = image_snap(img);
    image_t *hist;

    // Discard previous undo.
    while ((hist = img->history_next)) {
        DL_DELETE2(img->history, hist, history_prev, history_next);
        assert(hist != img->history_next);
        image_delete(hist);
    }

    DL_DELETE2(img->history, img,  history_prev, history_next);
    DL_APPEND2(img->history, snap, history_prev, history_next);
    DL_APPEND2(img->history, img,  history_prev, history_next);
    debug_print_history(img);
}

void image_history_resize(image_t *img, int size)
{
    int i, nb = 0;
    image_t *hist;
    layer_t *layer, *layer_tmp;

    // First cound the size of the history to compute how many we are going
    // to remove.
    for (hist = img->history; hist != img; hist = hist->history_next) nb++;
    nb = max(0, nb - size);
    for (i = 0; i < nb; i++) {
        hist = img->history;

        // XXX: do that in a function!
        DL_FOREACH_SAFE(hist->layers, layer, layer_tmp) {
            DL_DELETE(hist->layers, layer);
            layer_delete(layer);
        }
        DL_DELETE2(img->history, hist, history_prev, history_next);
        free(hist);
    }
}

// XXX: not clear what this is doing.  We should try to remove it.
// It swap the content of two images without touching their pointer or
// history.
static void swap(image_t *a, image_t *b)
{
    SWAP(*a, *b);
    SWAP(a->history, b->history);
    SWAP(a->history_next, b->history_next);
    SWAP(a->history_prev, b->history_prev);
}

void image_undo(image_t *img)
{
    image_t *prev = img->history_prev;
    if (img->history == img) {
        LOG_D("No more undo");
        return;
    }
    DL_DELETE2(img->history, img, history_prev, history_next);
    DL_PREPEND_ELEM2(img->history, prev, img, history_prev, history_next);
    swap(img, prev);

    // Don't move the camera for an undo.
    if (img->active_camera && prev->active_camera &&
            strcmp(img->active_camera->name, prev->active_camera->name) == 0) {
        camera_set(img->active_camera, prev->active_camera);
    }

    debug_print_history(img);
}

void image_redo(image_t *img)
{
    image_t *next = img->history_next;
    if (!next) {
        LOG_D("No more redo");
        return;
    }
    DL_DELETE2(img->history, next, history_prev, history_next);
    DL_PREPEND_ELEM2(img->history, img, next, history_prev, history_next);
    swap(img, next);
    debug_print_history(img);
}

bool image_layer_can_edit(const image_t *img, const layer_t *layer)
{
    return !layer->parent && !layer->image && !layer->shape;
}

/*
 * Function: image_get_key
 * Return a value that is garantied to change when the image change.
 */
uint32_t image_get_key(const image_t *img)
{
    uint32_t key = 0, k;
    layer_t *layer;
    camera_t *camera;
    material_t *material;

    DL_FOREACH(img->layers, layer) {
        k = layer_get_key(layer);
        key = XXH32(&k, sizeof(k), key);
    }
    DL_FOREACH(img->cameras, camera) {
        k = camera_get_key(camera);
        key = XXH32(&k, sizeof(k), key);
    }
    DL_FOREACH(img->materials, material) {
        k = material_get_hash(material);
        key = XXH32(&k, sizeof(k), key);
    }
    return key;
}

/*
 * Turn an image layer into a mesh of 1 voxel depth.
 */
static void image_image_layer_to_mesh(image_t *img, layer_t *layer)
{
    uint8_t *data;
    int i, j, w, h, bpp = 0, pos[3];
    uint8_t c[4];
    float p[3];
    img = img ?: goxel.image;
    layer = layer ?: img->active_layer;
    mesh_accessor_t acc;

    image_history_push(img);
    data = img_read(layer->image->path, &w, &h, &bpp);
    acc = mesh_get_accessor(layer->mesh);
    for (j = 0; j < w; j++)
    for (i = 0; i < h; i++) {
        vec3_set(p, i / (float)h - 0.5, 0.5 - j / (float)w, 0);
        mat4_mul_vec3(layer->mat, p, p);
        pos[0] = round(p[0]);
        pos[1] = round(p[1]);
        pos[2] = round(p[2]);
        memset(c, 0, 4);
        c[3] = 255;
        memcpy(c, data + (j * w + i) * bpp, bpp);
        mesh_set_at(layer->mesh, &acc, pos, c);
    }
    texture_delete(layer->image);
    layer->image = NULL;
    free(data);
}

ACTION_REGISTER(layer_clear,
    .help = "Clear the current layer",
    .script = "goxel.image.layers.active.volume.clear()",
    .icon = ICON_DELETE,
    .flags = ACTION_TOUCH_IMAGE,
    .default_shortcut = "Delete",
)

ACTION_REGISTER(img_new_layer,
    .help = "Add a new layer to the image",
    .script = "goxel.image.layers.add(new Layer())",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

ACTION_REGISTER(img_del_layer,
    .help = "Delete the active layer",
    .script = "goxel.image.layers.remove(goxel.image.layers.active)",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_REMOVE,
)

ACTION_REGISTER(img_move_layer_up,
    .help = "Move the active layer up",
    .script = "goxel.image.layers.move(goxel.image.layers.active, -1)",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_UPWARD,
)

ACTION_REGISTER(img_move_layer_down,
    .help = "Move the active layer down",
    .script = "goxel.image.layers.move(goxel.image.layers.active, +1)",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_DOWNWARD,
)

ACTION_REGISTER(img_duplicate_layer,
    .help = "Duplicate the active layer",
    .script = "goxel.image.layers.add(goxel.image.layers.active.copy())",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_clone_layer,
    .help = "Clone the active layer",
    .script = "goxel.image.layers.add(goxel.image.layers.active.clone())",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_unclone_layer,
    .help = "Unclone the active layer",
    .script = "goxel.image.layers.active.unclone()",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_select_parent_layer,
    .help = "Select the parent of a layer",
    .script = "let layers = goxel.image.layers;\n"
              "if (layers.active.parent)\n"
              "  layers.active = layers.active.parent\n",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_merge_visible_layers,
    .help = "Merge all the visible layers",
    .script = "goxel.image.mergeVisibleLayers()",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_new_camera,
    .help = "Add a new camera to the image",
    .script = "goxel.image.cameras.add(new Camera())",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

ACTION_REGISTER(img_del_camera,
    .help = "Delete the active camera",
    .cfunc = image_delete_camera,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_REMOVE,
)

ACTION_REGISTER(img_move_camera,
    .help = "Move the active camera",
    .cfunc = image_move_camera,
    .csig = "vppi",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_move_camera_up,
    .help = "Move the active camera up",
    .cfunc = image_move_camera_up,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_UPWARD,
)

ACTION_REGISTER(img_move_camera_down,
    .help = "Move the active camera down",
    .cfunc = image_move_camera_down,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_DOWNWARD,
)

ACTION_REGISTER(img_image_layer_to_mesh,
    .help = "Turn an image layer into a mesh",
    .cfunc = image_image_layer_to_mesh,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_new_shape_layer,
    .help = "Add a new shape layer to the image",
    .cfunc = image_add_shape_layer,
    .csig = "vp",
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(img_new_material,
    .help = "Add a new material to the image",
    .cfunc = image_add_material,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

ACTION_REGISTER(img_del_material,
    .help = "Delete a material",
    .cfunc = image_delete_material,
    .csig = "vpp",
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_REMOVE,
)

ACTION_REGISTER(img_auto_resize,
    .help = "Auto resize the image to fit the layers",
    .cfunc = image_auto_resize,
    .csig = "vp",
    .flags = ACTION_TOUCH_IMAGE,
)
