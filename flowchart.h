/**
 * flowchart.h - Editor de Fluxogramas com GTK4 e Cairo
 *
 * Estruturas de dados e declarações de funções para o editor.
 */

#ifndef FLOWCHART_H
#define FLOWCHART_H

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include <string.h>

/* ── Constantes ─────────────────────────────────────────────────────────── */

#define MAX_SHAPES      100
#define MAX_CONNECTORS  100
#define MAX_UNDO        50
#define SNAP_DISTANCE   15
#define MAX_TEXT_LEN    255   /* índice máximo válido para text[256] */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Enumerações ─────────────────────────────────────────────────────────── */

typedef enum {
    SHAPE_RECTANGLE,
    SHAPE_ROUNDED_RECT,
    SHAPE_DIAMOND,
    SHAPE_CIRCLE,
    SHAPE_TEXT
} ShapeType;

typedef enum {
    LINE_SOLID,
    LINE_DASHED,
    LINE_DOTTED
} LineStyle;

typedef enum {
    ARROW_NONE,
    ARROW_END,
    ARROW_START,
    ARROW_BOTH
} ArrowType;

typedef enum {
    RESIZE_NONE,
    RESIZE_TOP_LEFT,
    RESIZE_TOP,
    RESIZE_TOP_RIGHT,
    RESIZE_RIGHT,
    RESIZE_BOTTOM_RIGHT,
    RESIZE_BOTTOM,
    RESIZE_BOTTOM_LEFT,
    RESIZE_LEFT
} ResizeHandle;

typedef enum {
    ACTION_ADD_SHAPE,
    ACTION_ADD_CONNECTOR,
    ACTION_DELETE_SHAPE,
    ACTION_DELETE_CONNECTOR,
    ACTION_MOVE_SHAPE,
    ACTION_RESIZE_SHAPE,
    ACTION_CHANGE_TEXT,
    ACTION_CHANGE_COLOR
} ActionType;

/* ── Estruturas de dados ─────────────────────────────────────────────────── */

typedef struct {
    double x, y;
} Point;

typedef struct {
    ShapeType type;
    double x, y;
    double width, height;
    double r, g, b, a;  /* cor RGBA [0.0 – 1.0] */
    char   text[256];
    int    id;          /* identificador único */
} Shape;

typedef struct {
    int    start_shape; /* índice em AppState.shapes (-1 = livre) */
    int    end_shape;
    Point  start;
    Point  end;
    double r, g, b, a;
    double line_width;
    LineStyle style;
    ArrowType arrow;
} Connector;

/* Dados associados a cada entrada do histórico de undo */
typedef struct {
    ActionType type;
    union {
        Shape shape;

        struct {
            Connector connector;
            int       index;
        } conn_data;

        int shape_index;

        struct {
            int    shape_index;
            double old_x, old_y;
            double new_x, new_y;
        } move_data;

        struct {
            int    shape_index;
            double old_x,     old_y;
            double old_width, old_height;
            double new_x,     new_y;
            double new_width, new_height;
        } resize_data;

        struct {
            int  shape_index;
            char old_text[256];
            char new_text[256];
        } text_data;
    } data;
} UndoAction;

/* Estado global da aplicação */
typedef struct {
    /* Elementos do diagrama */
    Shape     shapes[MAX_SHAPES];
    int       shape_count;
    Connector connectors[MAX_CONNECTORS];
    int       connector_count;

    /* Histórico de undo (fila circular) */
    UndoAction undo_stack[MAX_UNDO];
    int        undo_count;
    int        undo_ptr;

    /* Seleção e interação */
    ShapeType    selected_shape_type;
    int          selected_shape;   /* -1 = nenhum */
    int          dragging_shape;   /* -1 = não está arrastando */
    int          resizing_shape;   /* -1 = não está redimensionando */
    ResizeHandle resize_handle;
    Point        resize_orig_pos;
    Point        resize_orig_size;
    Point        drag_offset;

    /* Criação de conectores */
    int   creating_connector;
    int   connector_start_shape;
    Point temp_connector_end;

    /* Modo borracha e edição de texto */
    int        eraser_mode;
    int        editing_text;
    GtkWidget *text_entry;

    /* Propriedades do pincel */
    double    color_r, color_g, color_b;
    double    line_width;
    LineStyle line_style;
    ArrowType arrow_type;

    /* Widgets GTK referenciados em callbacks */
    GtkWidget *drawing_area;
    GtkWidget *color_button;
    GtkWidget *width_spin;
} AppState;

/* ── Protótipos de funções ───────────────────────────────────────────────── */

/* Inicialização */
void init_app_state(AppState *state);

/* Formas */
void add_shape_to_app(AppState *state, double x, double y);
void delete_shape_from_app(AppState *state, int index);

/* Conectores */
void update_connector_positions(AppState *state);

/* Undo */
void add_undo_action(AppState *state, ActionType type, void *data);
void undo_last_action(AppState *state);

/* Renderização */
void draw_arrow(cairo_t *cr,
                double x1, double y1,
                double x2, double y2,
                int draw_start, int draw_end);
void draw_shape(cairo_t *cr, Shape *shape, int selected);
void draw_connector(cairo_t *cr, Connector *conn);

/* Geometria / hit-testing */
Point        get_shape_center(Shape *shape);
Point        get_shape_connection_point(Shape *shape, Point target);
int          find_shape_at(AppState *state, double x, double y);
ResizeHandle get_resize_handle_at(AppState *state, int shape_idx,
                                  double x, double y);

/* Persistência */
int save_project(AppState *state, const char *filename);
int load_project(AppState *state, const char *filename);

#endif /* FLOWCHART_H */
