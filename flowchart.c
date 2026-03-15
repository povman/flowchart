/*
 MIT License

 Copyright (c) 2026 Fábio Moraes

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction...
 *
 * flowchart.c - Lógica central do editor de fluxogramas
 *
 * Inclui: estado, undo/redo, renderização Cairo, hit-testing,
 * persistência em arquivo texto e todos os callbacks GTK4.
 */

#include "flowchart.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* ── Estado global ───────────────────────────────────────────────────────── */

static AppState  app_state;
static GtkWidget *text_dialog = NULL;
static GtkWidget *text_entry  = NULL;
static int        next_shape_id = 1;

/* ── Inicialização ───────────────────────────────────────────────────────── */

void init_app_state(AppState *state)
{
    memset(state, 0, sizeof(AppState));

    state->selected_shape_type   = SHAPE_RECTANGLE;
    state->selected_shape        = -1;
    state->dragging_shape        = -1;
    state->resizing_shape        = -1;
    state->resize_handle         = RESIZE_NONE;
    state->connector_start_shape = -1;

    state->color_r    = 0.2;
    state->color_g    = 0.6;
    state->color_b    = 0.9;
    state->line_width = 2.0;
    state->line_style = LINE_SOLID;
    state->arrow_type = ARROW_END;

    state->undo_ptr = -1;
}

/* ── Undo ────────────────────────────────────────────────────────────────── */

/**
 * Registra uma ação no histórico de undo.
 * Quando o buffer está cheio, a entrada mais antiga é descartada.
 */
void add_undo_action(AppState *state, ActionType type, void *data)
{
    if (state->undo_count >= MAX_UNDO) {
        /* Descarta a entrada mais antiga (índice 0) */
        memmove(&state->undo_stack[0],
                &state->undo_stack[1],
                (MAX_UNDO - 1) * sizeof(UndoAction));
        state->undo_count = MAX_UNDO - 1;
    }

    UndoAction *action = &state->undo_stack[state->undo_count++];
    action->type = type;

    switch (type) {
        case ACTION_ADD_SHAPE:
            action->data.shape = *(Shape *)data;
            break;

        case ACTION_ADD_CONNECTOR:
            action->data.conn_data.connector = *(Connector *)data;
            action->data.conn_data.index = state->connector_count - 1;
            break;

        case ACTION_DELETE_SHAPE:
        case ACTION_DELETE_CONNECTOR:
            action->data.shape_index = *(int *)data;
            break;

        case ACTION_MOVE_SHAPE: {
            struct { int idx; double ox, oy, nx, ny; } *m = data;
            action->data.move_data.shape_index = m->idx;
            action->data.move_data.old_x = m->ox;
            action->data.move_data.old_y = m->oy;
            action->data.move_data.new_x = m->nx;
            action->data.move_data.new_y = m->ny;
            break;
        }

        case ACTION_RESIZE_SHAPE: {
            struct {
                int idx;
                double ox, oy, ow, oh;
                double nx, ny, nw, nh;
            } *r = data;
            action->data.resize_data.shape_index = r->idx;
            action->data.resize_data.old_x      = r->ox;
            action->data.resize_data.old_y      = r->oy;
            action->data.resize_data.old_width  = r->ow;
            action->data.resize_data.old_height = r->oh;
            action->data.resize_data.new_x      = r->nx;
            action->data.resize_data.new_y      = r->ny;
            action->data.resize_data.new_width  = r->nw;
            action->data.resize_data.new_height = r->nh;
            break;
        }

        case ACTION_CHANGE_TEXT: {
            struct { int idx; char old_text[256]; char new_text[256]; } *t = data;
            action->data.text_data.shape_index = t->idx;
            /* memcpy é seguro aqui: ambos os buffers têm exatamente 256 bytes */
            memcpy(action->data.text_data.old_text, t->old_text, 256);
            memcpy(action->data.text_data.new_text, t->new_text, 256);
            break;
        }

        default:
            break;
    }
}

/**
 * Desfaz a última ação.
 * Implementação completa: reverte add, delete, move, resize e texto.
 */
void undo_last_action(AppState *state)
{
    if (state->undo_count == 0) return;

    UndoAction *action = &state->undo_stack[--state->undo_count];

    switch (action->type) {
        case ACTION_ADD_SHAPE:
            /* Remove a última forma adicionada */
            if (state->shape_count > 0)
                state->shape_count--;
        break;

        case ACTION_ADD_CONNECTOR:
            if (state->connector_count > 0)
                state->connector_count--;
        break;

        case ACTION_DELETE_SHAPE:
            /* Restaura a forma removida (se houver espaço) */
            if (state->shape_count < MAX_SHAPES) {
                state->shapes[state->shape_count++] = action->data.shape;
            }
            break;

        case ACTION_MOVE_SHAPE: {
            int idx = action->data.move_data.shape_index;
            if (idx >= 0 && idx < state->shape_count) {
                state->shapes[idx].x = action->data.move_data.old_x;
                state->shapes[idx].y = action->data.move_data.old_y;
                update_connector_positions(state);
            }
            break;
        }

        case ACTION_RESIZE_SHAPE: {
            int idx = action->data.resize_data.shape_index;
            if (idx >= 0 && idx < state->shape_count) {
                state->shapes[idx].x      = action->data.resize_data.old_x;
                state->shapes[idx].y      = action->data.resize_data.old_y;
                state->shapes[idx].width  = action->data.resize_data.old_width;
                state->shapes[idx].height = action->data.resize_data.old_height;
                update_connector_positions(state);
            }
            break;
        }

        case ACTION_CHANGE_TEXT: {
            int idx = action->data.text_data.shape_index;
            if (idx >= 0 && idx < state->shape_count) {
                strncpy(state->shapes[idx].text,
                        action->data.text_data.old_text,
                        MAX_TEXT_LEN);
                state->shapes[idx].text[MAX_TEXT_LEN] = '\0';
            }
            break;
        }

        default:
            break;
    }
}

/* ── Formas ──────────────────────────────────────────────────────────────── */

void add_shape_to_app(AppState *state, double x, double y)
{
    if (state->shape_count >= MAX_SHAPES) return;

    Shape *shape   = &state->shapes[state->shape_count++];
    shape->type    = state->selected_shape_type;
    shape->x       = x - 50.0;
    shape->y       = y - 35.0;
    shape->width   = 100.0;
    shape->height  = 70.0;
    shape->r       = state->color_r;
    shape->g       = state->color_g;
    shape->b       = state->color_b;
    shape->a       = 0.7;
    shape->text[0] = '\0';
    shape->id      = next_shape_id++;

    add_undo_action(state, ACTION_ADD_SHAPE, shape);
}

/**
 * Remove uma forma e todos os conectores que a referenciam.
 * Ajusta os índices dos conectores restantes.
 */
void delete_shape_from_app(AppState *state, int index)
{
    if (index < 0 || index >= state->shape_count) return;

    /* Remove conectores que tocam esta forma */
    for (int i = state->connector_count - 1; i >= 0; i--) {
        if (state->connectors[i].start_shape == index ||
            state->connectors[i].end_shape   == index) {
            for (int j = i; j < state->connector_count - 1; j++)
                state->connectors[j] = state->connectors[j + 1];
            state->connector_count--;
            }
    }

    /* Atualiza índices dos conectores restantes */
    for (int i = 0; i < state->connector_count; i++) {
        if (state->connectors[i].start_shape > index)
            state->connectors[i].start_shape--;
        if (state->connectors[i].end_shape > index)
            state->connectors[i].end_shape--;
    }

    /* Remove a forma do array */
    for (int i = index; i < state->shape_count - 1; i++)
        state->shapes[i] = state->shapes[i + 1];
    state->shape_count--;

    /* Atualiza seleção */
    if (state->selected_shape == index)
        state->selected_shape = -1;
    else if (state->selected_shape > index)
        state->selected_shape--;
}

/* ── Conectores ──────────────────────────────────────────────────────────── */

void update_connector_positions(AppState *state)
{
    for (int i = 0; i < state->connector_count; i++) {
        Connector *conn = &state->connectors[i];

        if (conn->start_shape < 0 || conn->start_shape >= state->shape_count) continue;
        if (conn->end_shape   < 0 || conn->end_shape   >= state->shape_count) continue;

        Point end_center   = get_shape_center(&state->shapes[conn->end_shape]);
        Point start_center = get_shape_center(&state->shapes[conn->start_shape]);

        conn->start = get_shape_connection_point(&state->shapes[conn->start_shape], end_center);
        conn->end   = get_shape_connection_point(&state->shapes[conn->end_shape],   start_center);
    }
}

/* ── Geometria ───────────────────────────────────────────────────────────── */

Point get_shape_center(Shape *shape)
{
    return (Point){ shape->x + shape->width  / 2.0,
        shape->y + shape->height / 2.0 };
}

/**
 * Retorna o ponto na borda da forma mais próximo de `target`.
 * Usado para ancorar conectores na superfície das formas.
 */
Point get_shape_connection_point(Shape *shape, Point target)
{
    Point center = get_shape_center(shape);
    Point result;

    if (shape->type == SHAPE_CIRCLE) {
        double radius = fmin(shape->width, shape->height) / 2.0;
        double angle  = atan2(target.y - center.y, target.x - center.x);
        result.x = center.x + radius * cos(angle);
        result.y = center.y + radius * sin(angle);

    } else if (shape->type == SHAPE_DIAMOND) {
        double half_w = shape->width  / 2.0;
        double half_h = shape->height / 2.0;
        double dx = target.x - center.x;
        double dy = target.y - center.y;
        double len = sqrt(dx * dx + dy * dy);

        if (len > 0.0) { dx /= len; dy /= len; }

        /* Interseção com a borda do losango usando parametrização */
        double t = (fabs(dx) > 1e-9 || fabs(dy) > 1e-9)
        ? fmin((fabs(dx) > 1e-9 ? half_w / fabs(dx) : G_MAXDOUBLE),
               (fabs(dy) > 1e-9 ? half_h / fabs(dy) : G_MAXDOUBLE))
        : 0.0;

        result.x = center.x + dx * t;
        result.y = center.y + dy * t;

    } else {
        /* Retângulo / retângulo arredondado */
        double dx = target.x - center.x;
        double dy = target.y - center.y;

        if (fabs(dx) > fabs(dy)) {
            double slope = (dx != 0.0) ? dy / dx : 0.0;
            if (dx > 0.0) {
                result.x = shape->x + shape->width;
                result.y = center.y + slope * (shape->width / 2.0);
            } else {
                result.x = shape->x;
                result.y = center.y - slope * (shape->width / 2.0);
            }
        } else {
            double slope = (dy != 0.0) ? dx / dy : 0.0;
            if (dy > 0.0) {
                result.y = shape->y + shape->height;
                result.x = center.x + slope * (shape->height / 2.0);
            } else {
                result.y = shape->y;
                result.x = center.x - slope * (shape->height / 2.0);
            }
        }

        /* Garante que o ponto esteja dentro do bounding-box */
        result.x = fmax(shape->x, fmin(shape->x + shape->width,  result.x));
        result.y = fmax(shape->y, fmin(shape->y + shape->height, result.y));
    }

    return result;
}

/**
 * Retorna o índice da forma sob (x, y), ou -1 se nenhuma.
 * Itera em ordem reversa para dar prioridade às formas do topo.
 */
int find_shape_at(AppState *state, double x, double y)
{
    for (int i = state->shape_count - 1; i >= 0; i--) {
        Shape *s = &state->shapes[i];

        if (s->type == SHAPE_DIAMOND) {
            double cx = s->x + s->width  / 2.0;
            double cy = s->y + s->height / 2.0;
            double dx = fabs(x - cx) / (s->width  / 2.0);
            double dy = fabs(y - cy) / (s->height / 2.0);
            if (dx + dy <= 1.0) return i;

        } else if (s->type == SHAPE_CIRCLE) {
            double cx     = s->x + s->width  / 2.0;
            double cy     = s->y + s->height / 2.0;
            double radius = fmin(s->width, s->height) / 2.0;
            double dist   = hypot(x - cx, y - cy);  /* hypot é mais preciso que sqrt(pow) */
            if (dist <= radius) return i;

        } else {
            if (x >= s->x && x <= s->x + s->width &&
                y >= s->y && y <= s->y + s->height)
                return i;
        }
    }
    return -1;
}

/**
 * Identifica qual alça de redimensionamento está sob (x, y).
 * Verifica primeiro as 8 alças de canto/meio, depois as bordas.
 */
ResizeHandle get_resize_handle_at(AppState *state, int shape_idx,
                                  double x, double y)
{
    if (shape_idx < 0 || shape_idx >= state->shape_count) return RESIZE_NONE;

    Shape  *shape       = &state->shapes[shape_idx];
    double  half_handle = 7.5;
    double  border_tol  = 8.0;

    struct { double x, y; ResizeHandle handle; } handles[] = {
        { shape->x,                      shape->y,                       RESIZE_TOP_LEFT     },
        { shape->x + shape->width / 2.0, shape->y,                       RESIZE_TOP          },
        { shape->x + shape->width,       shape->y,                       RESIZE_TOP_RIGHT    },
        { shape->x + shape->width,       shape->y + shape->height / 2.0, RESIZE_RIGHT        },
        { shape->x + shape->width,       shape->y + shape->height,       RESIZE_BOTTOM_RIGHT },
        { shape->x + shape->width / 2.0, shape->y + shape->height,       RESIZE_BOTTOM       },
        { shape->x,                      shape->y + shape->height,       RESIZE_BOTTOM_LEFT  },
        { shape->x,                      shape->y + shape->height / 2.0, RESIZE_LEFT         },
    };

    for (int i = 0; i < 8; i++) {
        if (fabs(x - handles[i].x) <= half_handle &&
            fabs(y - handles[i].y) <= half_handle)
            return handles[i].handle;
    }

    /* Bordas */
    if (fabs(x - shape->x)                  <= border_tol &&
        y >= shape->y && y <= shape->y + shape->height)    return RESIZE_LEFT;

    if (fabs(x - (shape->x + shape->width)) <= border_tol &&
        y >= shape->y && y <= shape->y + shape->height)    return RESIZE_RIGHT;

    if (fabs(y - shape->y)                   <= border_tol &&
        x >= shape->x && x <= shape->x + shape->width)    return RESIZE_TOP;

    if (fabs(y - (shape->y + shape->height)) <= border_tol &&
        x >= shape->x && x <= shape->x + shape->width)    return RESIZE_BOTTOM;

    return RESIZE_NONE;
}

/* ── Renderização ────────────────────────────────────────────────────────── */

void draw_arrow(cairo_t *cr,
                double x1, double y1,
                double x2, double y2,
                int draw_start, int draw_end)
{
    const double angle      = atan2(y2 - y1, x2 - x1);
    const double arrow_size = 12.0;

    if (draw_end) {
        cairo_save(cr);
        cairo_translate(cr, x2, y2);
        cairo_rotate(cr, angle);
        cairo_move_to(cr,  0,           0);
        cairo_line_to(cr, -arrow_size, -arrow_size / 2.0);
        cairo_line_to(cr, -arrow_size,  arrow_size / 2.0);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    if (draw_start) {
        cairo_save(cr);
        cairo_translate(cr, x1, y1);
        cairo_rotate(cr, angle + M_PI);
        cairo_move_to(cr,  0,           0);
        cairo_line_to(cr, -arrow_size, -arrow_size / 2.0);
        cairo_line_to(cr, -arrow_size,  arrow_size / 2.0);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

void draw_shape(cairo_t *cr, Shape *shape, int selected)
{
    cairo_save(cr);

    switch (shape->type) {
        case SHAPE_RECTANGLE:
            cairo_rectangle(cr, shape->x, shape->y, shape->width, shape->height);
            break;

        case SHAPE_ROUNDED_RECT: {
            const double radius = 10.0;
            cairo_new_sub_path(cr);
            cairo_arc(cr, shape->x + shape->width  - radius, shape->y              + radius, radius, -M_PI / 2.0, 0);
            cairo_arc(cr, shape->x + shape->width  - radius, shape->y + shape->height - radius, radius,  0,        M_PI / 2.0);
            cairo_arc(cr, shape->x              + radius, shape->y + shape->height - radius, radius,  M_PI / 2.0, M_PI);
            cairo_arc(cr, shape->x              + radius, shape->y              + radius, radius,  M_PI,      3.0 * M_PI / 2.0);
            cairo_close_path(cr);
            break;
        }

        case SHAPE_DIAMOND: {
            double cx = shape->x + shape->width  / 2.0;
            double cy = shape->y + shape->height / 2.0;
            cairo_move_to(cr, cx,                    shape->y);
            cairo_line_to(cr, shape->x + shape->width, cy);
            cairo_line_to(cr, cx,                    shape->y + shape->height);
            cairo_line_to(cr, shape->x,              cy);
            cairo_close_path(cr);
            break;
        }

        case SHAPE_CIRCLE: {
            double cx     = shape->x + shape->width  / 2.0;
            double cy     = shape->y + shape->height / 2.0;
            double radius = fmin(shape->width, shape->height) / 2.0;
            cairo_arc(cr, cx, cy, radius, 0, 2.0 * M_PI);
            break;
        }

        case SHAPE_TEXT:
            break;
    }

    if (shape->type != SHAPE_TEXT) {
        cairo_set_source_rgba(cr, shape->r, shape->g, shape->b, shape->a);
        cairo_fill_preserve(cr);

        if (selected)
            cairo_set_source_rgb(cr, 1.0, 0.5, 0.0);
        else
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

        cairo_set_line_width(cr, selected ? 3.0 : 2.0);
        cairo_stroke(cr);
    }

    /* Texto centralizado */
    if (shape->text[0] != '\0') {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_select_font_face(cr, "Sans",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, shape->text, &ext);

        cairo_move_to(cr,
                      shape->x + (shape->width  - ext.width)  / 2.0 - ext.x_bearing,
                      shape->y + (shape->height - ext.height) / 2.0 - ext.y_bearing);
        cairo_show_text(cr, shape->text);
    }

    /* Alças de redimensionamento (apenas quando selecionado) */
    if (selected) {
        const double hs = 8.0;
        const double hh = hs / 2.0;

        double hx[] = {
            shape->x,                      shape->x + shape->width / 2.0,
            shape->x + shape->width,       shape->x + shape->width,
            shape->x + shape->width,       shape->x + shape->width / 2.0,
            shape->x,                      shape->x
        };
        double hy[] = {
            shape->y,                      shape->y,
            shape->y,                      shape->y + shape->height / 2.0,
            shape->y + shape->height,      shape->y + shape->height,
            shape->y + shape->height,      shape->y + shape->height / 2.0
        };

        cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
        for (int i = 0; i < 8; i++) {
            cairo_rectangle(cr, hx[i] - hh, hy[i] - hh, hs, hs);
            cairo_fill(cr);
        }
    }

    cairo_restore(cr);
}

void draw_connector(cairo_t *cr, Connector *conn)
{
    cairo_save(cr);

    const double dashes[] = { 10.0, 5.0 };
    const double dots[]   = {  2.0, 3.0 };

    switch (conn->style) {
        case LINE_DASHED: cairo_set_dash(cr, dashes, 2, 0); break;
        case LINE_DOTTED: cairo_set_dash(cr, dots,   2, 0); break;
        default: break;
    }

    cairo_set_source_rgba(cr, conn->r, conn->g, conn->b, conn->a);
    cairo_set_line_width(cr, conn->line_width);
    cairo_move_to(cr, conn->start.x, conn->start.y);
    cairo_line_to(cr, conn->end.x,   conn->end.y);
    cairo_stroke(cr);

    draw_arrow(cr,
               conn->start.x, conn->start.y,
               conn->end.x,   conn->end.y,
               conn->arrow == ARROW_START || conn->arrow == ARROW_BOTH,
               conn->arrow == ARROW_END   || conn->arrow == ARROW_BOTH);

    cairo_restore(cr);
}

/* ── Persistência ────────────────────────────────────────────────────────── */

/**
 * Salva o projeto em formato texto simples (.flow).
 * Retorna 0 em caso de sucesso, -1 em erro.
 */
int save_project(AppState *state, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        g_printerr("Erro ao criar '%s': %s\n", filename, strerror(errno));
        return -1;
    }

    fprintf(fp, "# Flowchart Project File\n");
    fprintf(fp, "# Version: 1.0\n");
    fprintf(fp, "SHAPES:%d\n",     state->shape_count);
    fprintf(fp, "CONNECTORS:%d\n", state->connector_count);
    fprintf(fp, "COLOR:%.3f,%.3f,%.3f\n",
            state->color_r, state->color_g, state->color_b);
    fprintf(fp, "LINE_WIDTH:%.1f\n",  state->line_width);
    fprintf(fp, "LINE_STYLE:%d\n",   (int)state->line_style);
    fprintf(fp, "ARROW_TYPE:%d\n",   (int)state->arrow_type);

    fprintf(fp, "\n# Shapes\n");
    for (int i = 0; i < state->shape_count; i++) {
        Shape *s = &state->shapes[i];

        /* Escapa aspas e barras invertidas no texto */
        char escaped[512];
        int  pos = 0;
        for (int j = 0; s->text[j] && pos < 510; j++) {
            if (s->text[j] == '"' || s->text[j] == '\\')
                escaped[pos++] = '\\';
            escaped[pos++] = s->text[j];
        }
        escaped[pos] = '\0';

        fprintf(fp,
                "SHAPE:%d,%d,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,\"%s\",%d\n",
                i, (int)s->type, s->x, s->y, s->width, s->height,
                s->r, s->g, s->b, s->a, escaped, s->id);
    }

    fprintf(fp, "\n# Connectors\n");
    for (int i = 0; i < state->connector_count; i++) {
        Connector *c = &state->connectors[i];
        fprintf(fp,
                "CONNECTOR:%d,%d,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.1f,%d,%d\n",
                c->start_shape, c->end_shape,
                c->start.x, c->start.y, c->end.x, c->end.y,
                c->r, c->g, c->b, c->a,
                c->line_width, (int)c->style, (int)c->arrow);
    }

    fclose(fp);
    g_print("✅ Projeto salvo: %s (%d formas, %d conectores)\n",
            filename, state->shape_count, state->connector_count);
    return 0;
}

/**
 * Carrega um projeto a partir de arquivo .flow.
 * Retorna 0 em caso de sucesso, -1 em erro.
 */
int load_project(AppState *state, const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        g_printerr("Erro ao abrir '%s': %s\n", filename, strerror(errno));
        return -1;
    }

    /* Reseta estado */
    state->shape_count        = 0;
    state->connector_count    = 0;
    state->selected_shape     = -1;
    state->dragging_shape     = -1;
    state->resizing_shape     = -1;
    state->resize_handle      = RESIZE_NONE;
    state->creating_connector = 0;
    state->connector_start_shape = -1;

    char line[1024];
    int  shapes_loaded = 0, connectors_loaded = 0;
    int  expected_shapes = 0, expected_connectors = 0;
    int  max_id = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Remove \n e \r */
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;

        if      (strncmp(line, "SHAPES:",      7)  == 0) expected_shapes      = atoi(line + 7);
        else if (strncmp(line, "CONNECTORS:", 11)  == 0) expected_connectors  = atoi(line + 11);
        else if (strncmp(line, "COLOR:",       6)  == 0)
            sscanf(line + 6, "%lf,%lf,%lf",
                   &state->color_r, &state->color_g, &state->color_b);
            else if (strncmp(line, "LINE_WIDTH:", 11) == 0) state->line_width = atof(line + 11);
            else if (strncmp(line, "LINE_STYLE:", 11) == 0) state->line_style = (LineStyle)atoi(line + 11);
            else if (strncmp(line, "ARROW_TYPE:", 11) == 0) state->arrow_type = (ArrowType)atoi(line + 11);

            else if (strncmp(line, "SHAPE:", 6) == 0) {
                if (state->shape_count >= MAX_SHAPES) {
                    g_printerr("Aviso: limite de formas atingido, ignorando linha.\n");
                    continue;
                }

                Shape *s = &state->shapes[state->shape_count];
                memset(s, 0, sizeof(Shape));

                char *ptr = line + 6;
                int   type, id;

                strtol(ptr, &ptr, 10); ptr++;          /* índice (ignorado) */
                type  = (int)strtol(ptr, &ptr, 10); ptr++;
                s->x      = strtod(ptr, &ptr); ptr++;
                s->y      = strtod(ptr, &ptr); ptr++;
                s->width  = strtod(ptr, &ptr); ptr++;
                s->height = strtod(ptr, &ptr); ptr++;
                s->r = strtod(ptr, &ptr); ptr++;
                s->g = strtod(ptr, &ptr); ptr++;
                s->b = strtod(ptr, &ptr); ptr++;
                s->a = strtod(ptr, &ptr); ptr++;

                if (*ptr == '"') {
                    ptr++;
                    int tp = 0;
                    while (*ptr && *ptr != '"' && tp < MAX_TEXT_LEN) {
                        if (*ptr == '\\' && *(ptr + 1) != '\0') ptr++;
                        s->text[tp++] = *ptr++;
                    }
                    s->text[tp] = '\0';
                    if (*ptr == '"') ptr++;
                    if (*ptr == ',') ptr++;
                }

                id   = (int)strtol(ptr, NULL, 10);
                s->type = (ShapeType)type;
                s->id   = id;
                if (id > max_id) max_id = id;

                state->shape_count++;
                shapes_loaded++;

            } else if (strncmp(line, "CONNECTOR:", 10) == 0) {
                if (state->connector_count >= MAX_CONNECTORS) {
                    g_printerr("Aviso: limite de conectores atingido, ignorando linha.\n");
                    continue;
                }

                Connector *c = &state->connectors[state->connector_count];
                memset(c, 0, sizeof(Connector));

                int style, arrow;
                sscanf(line + 10,
                       "%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d,%d",
                       &c->start_shape, &c->end_shape,
                       &c->start.x, &c->start.y,
                       &c->end.x,   &c->end.y,
                       &c->r, &c->g, &c->b, &c->a,
                       &c->line_width, &style, &arrow);

                c->style = (LineStyle)style;
                c->arrow = (ArrowType)arrow;

                state->connector_count++;
                connectors_loaded++;
            }
    }

    fclose(fp);
    update_connector_positions(state);
    next_shape_id = max_id + 1;

    g_print("✅ Projeto carregado: %s (%d/%d formas, %d/%d conectores)\n",
            filename,
            shapes_loaded,      expected_shapes,
            connectors_loaded,  expected_connectors);
    return 0;
}

/* ── Callbacks de desenho ────────────────────────────────────────────────── */

static void draw_function(GtkDrawingArea *area, cairo_t *cr,
                          int width, int height, gpointer data)
{
    (void)area; (void)width; (void)height; (void)data;

    /* Fundo branco */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* Conectores (abaixo das formas) */
    for (int i = 0; i < app_state.connector_count; i++)
        draw_connector(cr, &app_state.connectors[i]);

    /* Preview do conector sendo criado */
    if (app_state.creating_connector && app_state.connector_start_shape >= 0) {
        Point target = app_state.temp_connector_end;
        Point start  = get_shape_connection_point(
            &app_state.shapes[app_state.connector_start_shape], target);

        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.7);
        cairo_set_line_width(cr, app_state.line_width);
        cairo_move_to(cr, start.x, start.y);
        cairo_line_to(cr, target.x, target.y);
        cairo_stroke(cr);
    }

    /* Formas */
    for (int i = 0; i < app_state.shape_count; i++)
        draw_shape(cr, &app_state.shapes[i], i == app_state.selected_shape);
}

/* ── Diálogo de edição de texto ──────────────────────────────────────────── */

static void on_text_dialog_ok(GtkButton *button, gpointer data)
{
    (void)button; (void)data;

    if (text_entry && app_state.selected_shape >= 0 &&
        app_state.selected_shape < app_state.shape_count) {

        const char *txt = gtk_editable_get_text(GTK_EDITABLE(text_entry));
    strncpy(app_state.shapes[app_state.selected_shape].text, txt, MAX_TEXT_LEN);
    app_state.shapes[app_state.selected_shape].text[MAX_TEXT_LEN] = '\0';
    gtk_widget_queue_draw(app_state.drawing_area);
        }

        if (text_dialog) {
            gtk_window_destroy(GTK_WINDOW(text_dialog));
            text_dialog = NULL;
            text_entry  = NULL;
        }
}

static void on_text_dialog_cancel(GtkButton *button, gpointer data)
{
    (void)button; (void)data;

    if (text_dialog) {
        gtk_window_destroy(GTK_WINDOW(text_dialog));
        text_dialog = NULL;
        text_entry  = NULL;
    }
}

static gboolean on_dialog_key_press(GtkEventControllerKey *self,
                                    guint keyval, guint keycode,
                                    GdkModifierType state, gpointer data)
{
    (void)self; (void)keycode; (void)state; (void)data;

    if (keyval == GDK_KEY_Escape) {
        on_text_dialog_cancel(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

static void show_text_edit_dialog(int shape_idx)
{
    if (text_dialog) gtk_window_destroy(GTK_WINDOW(text_dialog));
    if (shape_idx < 0 || shape_idx >= app_state.shape_count) return;

    app_state.selected_shape = shape_idx;

    GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(app_state.drawing_area));
    text_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(text_dialog), "Editar Texto");
    gtk_window_set_transient_for(GTK_WINDOW(text_dialog), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(text_dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(text_dialog), 400, 150);
    gtk_window_set_resizable(GTK_WINDOW(text_dialog), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top   (box, 15);
    gtk_widget_set_margin_bottom(box, 15);
    gtk_widget_set_margin_start (box, 15);
    gtk_widget_set_margin_end   (box, 15);

    gtk_box_append(GTK_BOX(box), gtk_label_new("Digite o texto para a forma:"));

    text_entry = gtk_entry_new();
    if (app_state.shapes[shape_idx].text[0] != '\0') {
        gtk_editable_set_text(GTK_EDITABLE(text_entry),
                              app_state.shapes[shape_idx].text);
    }
    gtk_box_append(GTK_BOX(box), text_entry);

    GtkWidget *btn_box     = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *cancel_btn  = gtk_button_new_with_label("Cancelar");
    GtkWidget *ok_btn      = gtk_button_new_with_label("OK");

    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_hexpand(btn_box, TRUE);
    gtk_widget_set_size_request(cancel_btn, 100, -1);
    gtk_widget_set_size_request(ok_btn,     100, -1);

    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(box), btn_box);
    gtk_window_set_child(GTK_WINDOW(text_dialog), box);

    g_signal_connect(cancel_btn, "clicked",  G_CALLBACK(on_text_dialog_cancel), NULL);
    g_signal_connect(ok_btn,     "clicked",  G_CALLBACK(on_text_dialog_ok),     NULL);
    g_signal_connect(text_entry, "activate", G_CALLBACK(on_text_dialog_ok),     NULL);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_dialog_key_press), NULL);
    gtk_widget_add_controller(text_dialog, kc);

    gtk_widget_grab_focus(text_entry);
    gtk_window_present(GTK_WINDOW(text_dialog));
}

/* ── Callbacks de arquivo ────────────────────────────────────────────────── */

static void on_save_dialog_response(GtkFileDialog *dialog,
                                    GAsyncResult *result, gpointer data)
{
    (void)data;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            save_project(&app_state, path);
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void on_open_dialog_response(GtkFileDialog *dialog,
                                    GAsyncResult *result, gpointer data)
{
    (void)data;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            if (load_project(&app_state, path) == 0)
                gtk_widget_queue_draw(app_state.drawing_area);
            g_free(path);
        }
        g_object_unref(file);
    }
}

static GtkFileFilter *make_flow_filter(void)
{
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Arquivos Flowchart (*.flow)");
    gtk_file_filter_add_pattern(f, "*.flow");
    gtk_file_filter_add_pattern(f, "*.txt");
    return f;
}

static void on_save_project(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    GtkWidget    *window = GTK_WIDGET(gtk_widget_get_root(app_state.drawing_area));
    GtkFileDialog *dlg   = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Salvar Projeto");
    gtk_file_dialog_set_initial_name(dlg, "meu_projeto.flow");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, make_flow_filter());
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));

    gtk_file_dialog_save(dlg, GTK_WINDOW(window), NULL,
                         (GAsyncReadyCallback)on_save_dialog_response, NULL);
}

static void on_load_project(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    GtkWidget    *window = GTK_WIDGET(gtk_widget_get_root(app_state.drawing_area));
    GtkFileDialog *dlg   = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Abrir Projeto");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, make_flow_filter());
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));

    gtk_file_dialog_open(dlg, GTK_WINDOW(window), NULL,
                         (GAsyncReadyCallback)on_open_dialog_response, NULL);
}

/* ── Callbacks de eventos de entrada ─────────────────────────────────────── */

static void on_click(GtkGestureClick *gesture, int n_press,
                     double x, double y, gpointer data)
{
    (void)gesture;
    (void)data;
    int shape_idx = find_shape_at(&app_state, x, y);

    /* Modo borracha */
    if (app_state.eraser_mode) {
        if (shape_idx >= 0) {
            delete_shape_from_app(&app_state, shape_idx);
            gtk_widget_queue_draw(app_state.drawing_area);
        }
        return;
    }

    /* Finaliza criação de conector */
    if (app_state.creating_connector) {
        if (shape_idx >= 0 && shape_idx != app_state.connector_start_shape &&
            app_state.connector_count < MAX_CONNECTORS) {

            Connector *conn        = &app_state.connectors[app_state.connector_count++];
        conn->start_shape      = app_state.connector_start_shape;
        conn->end_shape        = shape_idx;
        conn->r                = app_state.color_r;
        conn->g                = app_state.color_g;
        conn->b                = app_state.color_b;
        conn->a                = 1.0;
        conn->line_width       = app_state.line_width;
        conn->style            = app_state.line_style;
        conn->arrow            = app_state.arrow_type;

        Point sc = get_shape_center(&app_state.shapes[conn->start_shape]);
        Point ec = get_shape_center(&app_state.shapes[conn->end_shape]);
        conn->start = get_shape_connection_point(&app_state.shapes[conn->start_shape], ec);
        conn->end   = get_shape_connection_point(&app_state.shapes[conn->end_shape],   sc);

        add_undo_action(&app_state, ACTION_ADD_CONNECTOR, conn);
            }
            app_state.creating_connector  = 0;
            app_state.connector_start_shape = -1;
            gtk_widget_queue_draw(app_state.drawing_area);
            return;
    }

    /* Edição por duplo-clique */
    if (shape_idx >= 0 && n_press == 2) {
        show_text_edit_dialog(shape_idx);
        return;
    }

    /* Seleção ou redimensionamento */
    app_state.selected_shape = shape_idx;

    if (shape_idx >= 0) {
        ResizeHandle handle = get_resize_handle_at(&app_state, shape_idx, x, y);
        if (handle != RESIZE_NONE) {
            app_state.resizing_shape     = shape_idx;
            app_state.resize_handle      = handle;
            app_state.resize_orig_pos.x  = x;
            app_state.resize_orig_pos.y  = y;
            app_state.resize_orig_size.x = app_state.shapes[shape_idx].width;
            app_state.resize_orig_size.y = app_state.shapes[shape_idx].height;
        } else {
            app_state.dragging_shape  = shape_idx;
            app_state.drag_offset.x   = x - app_state.shapes[shape_idx].x;
            app_state.drag_offset.y   = y - app_state.shapes[shape_idx].y;
        }
    } else {
        /* Clique em área vazia: cria nova forma */
        add_shape_to_app(&app_state, x, y);
    }

    gtk_widget_queue_draw(app_state.drawing_area);
}

static void on_release(GtkGestureClick *gesture, int n_press,
                       double x, double y, gpointer data)
{
    (void)gesture; (void)n_press; (void)x; (void)y; (void)data;
    app_state.dragging_shape = -1;
    app_state.resizing_shape = -1;
    app_state.resize_handle  = RESIZE_NONE;
}

static void on_drag(GtkGestureDrag *gesture,
                    double offset_x, double offset_y, gpointer data)
{
    (void)data;

    if (app_state.dragging_shape >= 0) {
        double sx, sy;
        gtk_gesture_drag_get_start_point(gesture, &sx, &sy);

        Shape *shape = &app_state.shapes[app_state.dragging_shape];
        shape->x = sx + offset_x - app_state.drag_offset.x;
        shape->y = sy + offset_y - app_state.drag_offset.y;

        update_connector_positions(&app_state);
        gtk_widget_queue_draw(app_state.drawing_area);

    } else if (app_state.resizing_shape >= 0) {
        double sx, sy;
        gtk_gesture_drag_get_start_point(gesture, &sx, &sy);

        double cx    = sx + offset_x;
        double cy    = sy + offset_y;
        double delta_x = cx - app_state.resize_orig_pos.x;
        double delta_y = cy - app_state.resize_orig_pos.y;
        double ow    = app_state.resize_orig_size.x;
        double oh    = app_state.resize_orig_size.y;

        Shape *shape = &app_state.shapes[app_state.resizing_shape];

        #define MIN_DIM 20.0
        switch (app_state.resize_handle) {
            case RESIZE_TOP_LEFT:
                shape->x      += delta_x;
                shape->y      += delta_y;
                shape->width   = fmax(MIN_DIM, ow - delta_x);
                shape->height  = fmax(MIN_DIM, oh - delta_y);
                break;
            case RESIZE_TOP:
                shape->y      += delta_y;
                shape->height  = fmax(MIN_DIM, oh - delta_y);
                break;
            case RESIZE_TOP_RIGHT:
                shape->y      += delta_y;
                shape->width   = fmax(MIN_DIM, ow + delta_x);
                shape->height  = fmax(MIN_DIM, oh - delta_y);
                break;
            case RESIZE_RIGHT:
                shape->width   = fmax(MIN_DIM, ow + delta_x);
                break;
            case RESIZE_BOTTOM_RIGHT:
                shape->width   = fmax(MIN_DIM, ow + delta_x);
                shape->height  = fmax(MIN_DIM, oh + delta_y);
                break;
            case RESIZE_BOTTOM:
                shape->height  = fmax(MIN_DIM, oh + delta_y);
                break;
            case RESIZE_BOTTOM_LEFT:
                shape->x      += delta_x;
                shape->width   = fmax(MIN_DIM, ow - delta_x);
                shape->height  = fmax(MIN_DIM, oh + delta_y);
                break;
            case RESIZE_LEFT:
                shape->x      += delta_x;
                shape->width   = fmax(MIN_DIM, ow - delta_x);
                break;
            case RESIZE_NONE:
                break;
        }
        #undef MIN_DIM

        update_connector_positions(&app_state);
        gtk_widget_queue_draw(app_state.drawing_area);
    }
}

static void on_motion(GtkEventControllerMotion *controller,
                      double x, double y, gpointer data)
{
    (void)controller; (void)data;

    if (app_state.creating_connector) {
        app_state.temp_connector_end.x = x;
        app_state.temp_connector_end.y = y;
        gtk_widget_queue_draw(app_state.drawing_area);
    }

    /* Atualiza cursor */
    const char *cursor_name = "default";
    int shape_idx = find_shape_at(&app_state, x, y);

    if (shape_idx >= 0) {
        ResizeHandle handle = get_resize_handle_at(&app_state, shape_idx, x, y);
        switch (handle) {
            case RESIZE_TOP_LEFT:  case RESIZE_BOTTOM_RIGHT: cursor_name = "nwse-resize"; break;
            case RESIZE_TOP_RIGHT: case RESIZE_BOTTOM_LEFT:  cursor_name = "nesw-resize"; break;
            case RESIZE_TOP:       case RESIZE_BOTTOM:        cursor_name = "ns-resize";   break;
            case RESIZE_LEFT:      case RESIZE_RIGHT:         cursor_name = "ew-resize";   break;
            default: cursor_name = "default"; break;
        }
    }

    GdkCursor  *cursor  = gdk_cursor_new_from_name(cursor_name, NULL);
    GdkSurface *surface = gtk_native_get_surface(
        gtk_widget_get_native(app_state.drawing_area));
    if (cursor && surface) {
        gdk_surface_set_cursor(surface, cursor);
        g_object_unref(cursor);
    }
}

/* ── Callbacks da barra de ferramentas ───────────────────────────────────── */

static void on_shape_button_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    app_state.selected_shape_type = GPOINTER_TO_INT(data);
    app_state.eraser_mode         = 0;
}

static void on_eraser_button_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    app_state.eraser_mode         = 1;
    app_state.creating_connector  = 0;
}

static void on_connector_button_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (app_state.selected_shape >= 0) {
        app_state.creating_connector    = 1;
        app_state.connector_start_shape = app_state.selected_shape;
        app_state.eraser_mode           = 0;
    } else {
        g_print("Selecione uma forma primeiro!\n");
    }
}

static void on_text_button_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (app_state.selected_shape >= 0)
        show_text_edit_dialog(app_state.selected_shape);
    else
        g_print("Selecione uma forma primeiro!\n");
}

static void on_color_set(GtkColorDialogButton *button, gpointer data)
{
    (void)data;
    const GdkRGBA *color = gtk_color_dialog_button_get_rgba(button);
    if (color) {
        app_state.color_r = color->red;
        app_state.color_g = color->green;
        app_state.color_b = color->blue;
    }
}

static void on_width_changed(GtkSpinButton *button, gpointer data)
{
    (void)data;
    app_state.line_width = gtk_spin_button_get_value(button);
}

static void on_line_style_changed(GtkDropDown *dropdown,
                                  GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    app_state.line_style = (LineStyle)gtk_drop_down_get_selected(dropdown);
}

static void on_arrow_type_changed(GtkDropDown *dropdown,
                                  GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    app_state.arrow_type = (ArrowType)gtk_drop_down_get_selected(dropdown);
}

static void save_to_png(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    int w = gtk_widget_get_width(app_state.drawing_area);
    int h = gtk_widget_get_height(app_state.drawing_area);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t         *cr   = cairo_create(surf);

    draw_function(NULL, cr, w, h, NULL);

    cairo_surface_write_to_png(surf, "flowchart.png");
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    g_print("Salvo em flowchart.png\n");
}

static gboolean on_key_press(GtkEventControllerKey *controller,
                             guint keyval, guint keycode,
                             GdkModifierType state, gpointer data)
{
    (void)controller; (void)keycode; (void)data;

    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
        undo_last_action(&app_state);
    gtk_widget_queue_draw(app_state.drawing_area);
    return TRUE;
        }

        if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) &&
            app_state.selected_shape >= 0) {
            delete_shape_from_app(&app_state, app_state.selected_shape);
        gtk_widget_queue_draw(app_state.drawing_area);
        return TRUE;
            }

            if ((keyval == GDK_KEY_e || keyval == GDK_KEY_E) &&
                app_state.selected_shape >= 0) {
                show_text_edit_dialog(app_state.selected_shape);
            return TRUE;
                }

                if ((state & GDK_CONTROL_MASK) &&
                    (keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
                    on_save_project(NULL, NULL);
                return TRUE;
                    }

                    if ((state & GDK_CONTROL_MASK) &&
                        (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) {
                        on_load_project(NULL, NULL);
                    return TRUE;
                        }

                        return FALSE;
}

/* ── Construção da janela principal ──────────────────────────────────────── */

static void activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    init_app_state(&app_state);

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Editor de Flowchart");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), main_box);

    /* ── Painel esquerdo ── */
    GtkWidget *left_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(left_panel, 150, -1);
    gtk_box_append(GTK_BOX(main_box), left_panel);

    gtk_box_append(GTK_BOX(left_panel), gtk_label_new("Formas"));

    const char *shape_names[] = { "Retângulo", "Ret. Arred.", "Losango", "Círculo", "Texto" };
    for (int i = 0; i < 5; i++) {
        GtkWidget *btn = gtk_button_new_with_label(shape_names[i]);
        g_signal_connect(btn, "clicked",
                         G_CALLBACK(on_shape_button_clicked), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(left_panel), btn);
    }

    GtkWidget *conn_btn = gtk_button_new_with_label("Adicionar Conector");
    g_signal_connect(conn_btn, "clicked", G_CALLBACK(on_connector_button_clicked), NULL);
    gtk_box_append(GTK_BOX(left_panel), conn_btn);

    GtkWidget *eraser_btn = gtk_button_new_with_label("🗑️ Borracha");
    g_signal_connect(eraser_btn, "clicked", G_CALLBACK(on_eraser_button_clicked), NULL);
    gtk_box_append(GTK_BOX(left_panel), eraser_btn);

    GtkWidget *text_btn = gtk_button_new_with_label("✏️ Editar Texto");
    g_signal_connect(text_btn, "clicked", G_CALLBACK(on_text_button_clicked), NULL);
    gtk_box_append(GTK_BOX(left_panel), text_btn);

    GtkWidget *save_proj_btn = gtk_button_new_with_label("💾 Salvar Projeto");
    g_signal_connect(save_proj_btn, "clicked", G_CALLBACK(on_save_project), NULL);
    gtk_box_append(GTK_BOX(left_panel), save_proj_btn);

    GtkWidget *load_proj_btn = gtk_button_new_with_label("📂 Abrir Projeto");
    g_signal_connect(load_proj_btn, "clicked", G_CALLBACK(on_load_project), NULL);
    gtk_box_append(GTK_BOX(left_panel), load_proj_btn);

    /* ── Área de desenho ── */
    app_state.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app_state.drawing_area, TRUE);
    gtk_widget_set_vexpand(app_state.drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app_state.drawing_area),
                                   draw_function, NULL, NULL);
    gtk_box_append(GTK_BOX(main_box), app_state.drawing_area);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed",  G_CALLBACK(on_click),   NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_release), NULL);
    gtk_widget_add_controller(app_state.drawing_area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag), NULL);
    gtk_widget_add_controller(app_state.drawing_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
    gtk_widget_add_controller(app_state.drawing_area, motion);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_press), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), key);

    /* ── Painel direito (propriedades) ── */
    GtkWidget *right_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(right_panel, 200, -1);
    gtk_box_append(GTK_BOX(main_box), right_panel);

    gtk_box_append(GTK_BOX(right_panel), gtk_label_new("Propriedades"));
    gtk_box_append(GTK_BOX(right_panel), gtk_label_new("Cor:"));

    GdkRGBA           initial_color = { 0.2, 0.6, 0.9, 1.0 };
    GtkColorDialog   *color_dlg     = gtk_color_dialog_new();
    app_state.color_button          = gtk_color_dialog_button_new(color_dlg);
    gtk_color_dialog_button_set_rgba(
        GTK_COLOR_DIALOG_BUTTON(app_state.color_button), &initial_color);
    g_signal_connect(app_state.color_button, "notify::rgba",
                     G_CALLBACK(on_color_set), NULL);
    gtk_box_append(GTK_BOX(right_panel), app_state.color_button);

    gtk_box_append(GTK_BOX(right_panel), gtk_label_new("Largura da Linha:"));
    app_state.width_spin = gtk_spin_button_new_with_range(1.0, 10.0, 0.5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app_state.width_spin), 2.0);
    g_signal_connect(app_state.width_spin, "value-changed",
                     G_CALLBACK(on_width_changed), NULL);
    gtk_box_append(GTK_BOX(right_panel), app_state.width_spin);

    gtk_box_append(GTK_BOX(right_panel), gtk_label_new("Estilo da Linha:"));
    const char *line_styles[] = { "Sólida", "Tracejada", "Pontilhada", NULL };
    GtkWidget  *line_dd       = gtk_drop_down_new_from_strings(line_styles);
    g_signal_connect(line_dd, "notify::selected",
                     G_CALLBACK(on_line_style_changed), NULL);
    gtk_box_append(GTK_BOX(right_panel), line_dd);

    gtk_box_append(GTK_BOX(right_panel), gtk_label_new("Tipo de Seta:"));
    const char *arrow_types[] = { "Sem Seta", "Fim", "Início", "Ambos", NULL };
    GtkWidget  *arrow_dd      = gtk_drop_down_new_from_strings(arrow_types);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(arrow_dd), 1);
    g_signal_connect(arrow_dd, "notify::selected",
                     G_CALLBACK(on_arrow_type_changed), NULL);
    gtk_box_append(GTK_BOX(right_panel), arrow_dd);

    GtkWidget *save_png_btn = gtk_button_new_with_label("Salvar PNG");
    g_signal_connect(save_png_btn, "clicked", G_CALLBACK(save_to_png), NULL);
    gtk_box_append(GTK_BOX(right_panel), save_png_btn);

    gtk_box_append(GTK_BOX(right_panel), gtk_label_new(
        "Ctrl+Z: Desfazer\n"
        "Delete: Apagar\n"
        "Duplo clique: Editar texto\n"
        "Tecla E: Editar texto\n"
        "Arrastar: Mover formas\n"
        "Bordas: Redimensionar\n"
        "Ctrl+S: Salvar\n"
        "Ctrl+O: Abrir"
    ));

    gtk_window_present(GTK_WINDOW(window));
}

/* ── Ponto de entrada ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("com.flowchart.editor",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
