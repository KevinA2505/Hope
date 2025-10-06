// domino.c — Planificador por mesa (FCFS / SJF_POINTS / SJF_PLAYERS / RR) + 1 acción por turno
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#define MAX_PLAYERS 4
#define MAX_TILES 28
#define ACTION_Q_CAP 1024
#define DEFAULT_TURN_COOLDOWN_MS 0 // enfriamiento configurable por turno planificado

typedef enum
{
    FCFS,
    SJF_PLAYERS,
    SJF_POINTS,
    RR
} policy_t;

static const char *policy_name(policy_t p)
{
    switch (p)
    {
    case FCFS:
        return "FCFS";
    case SJF_PLAYERS:
        return "SJF_PLAYERS";
    case SJF_POINTS:
        return "SJF_POINTS";
    case RR:
        return "RR";
    }
    return "?";
}
typedef enum
{
    NEW,
    READY,
    RUNNING,
    IO_WAIT,
    TERMINATED
} pstate_t;

typedef struct
{
    int a, b;
} tile_t;

typedef struct
{
    tile_t train[128];
    int train_len;
    int left_end, right_end;
    tile_t hands[MAX_PLAYERS][14];
    int hand_len[MAX_PLAYERS];
    tile_t pool[28];
    int pool_len;

    int nplayers, turn, table_id, finished;
    int steps, max_steps;
    int pass_streak;

    // NUEVO: planificación y sincronización de turnos
    policy_t policy;
    int rr_quantum_ms; // reservado para permitir N acciones por quantum en el futuro
    int turn_cooldown_ms;
    int action_done;   // lo setea el validador tras aplicar una acción

    pthread_mutex_t mtx;
    pthread_cond_t cv;
} game_state_t;

typedef struct
{
    int pid, table_id, player_id;
    pstate_t st;
    policy_t pol;
    long arrival_ms, first_run_ms, finish_ms;
    long wait_ready_ms, wait_io_ms;
    long runs, io_ops;
} pcb_t;

/* ===== control en caliente: prototipos ===== */
struct game_state_s;                                    // fwd si deseas; aquí no es estrictamente necesario
static int all_tables_finished(game_state_t *t, int n); // ya existe más abajo: solo prototipo

typedef struct
{
    game_state_t *tables;
    int n_tables;
} control_args_t;

void *control_thread(void *arg);

/* ===== util ===== */
static inline int is_double(tile_t t) { return t.a == t.b; }
static inline int tile_sum(tile_t t) { return t.a + t.b; }
static void print_tile(tile_t t) { printf("[%d|%d]", t.a, t.b); }

static void sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    struct timespec rem = {0};
    while (nanosleep(&ts, &rem) == -1 && errno == EINTR)
        ts = rem;
}

static int hand_points(game_state_t *g, int pid)
{
    int s = 0;
    for (int i = 0; i < g->hand_len[pid]; ++i)
        s += g->hands[pid][i].a + g->hands[pid][i].b;
    return s;
}
static int winner_lowest_points(game_state_t *g)
{
    int win = -1, best_pts = INT_MAX, best_tiles = INT_MAX;
    for (int p = 0; p < g->nplayers; p++)
    {
        int pts = hand_points(g, p), tiles = g->hand_len[p];
        if (pts < best_pts || (pts == best_pts && tiles < best_tiles) ||
            (pts == best_pts && tiles == best_tiles && p < win))
        {
            win = p;
            best_pts = pts;
            best_tiles = tiles;
        }
    }
    return win;
}
static void print_points_table(game_state_t *g)
{
    printf("---- Puntajes de cierre (mesa %d) ----\n", g->table_id);
    for (int p = 0; p < g->nplayers; p++)
        printf("J%d: %d puntos (%d fichas)\n", p, hand_points(g, p), g->hand_len[p]);
}

/* ===== mazo / reparto ===== */
static void build_deck(tile_t d[MAX_TILES], int *len)
{
    int k = 0;
    for (int i = 0; i <= 6; i++)
        for (int j = i; j <= 6; j++)
            d[k++] = (tile_t){i, j};
    *len = k;
}
static void shuffle_deck(tile_t d[], int len)
{
    for (int i = len - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        tile_t t = d[i];
        d[i] = d[j];
        d[j] = t;
    }
}
static tile_t take_from_hand(game_state_t *g, int p, int idx)
{
    tile_t t = g->hands[p][idx];
    for (int i = idx + 1; i < g->hand_len[p]; ++i)
        g->hands[p][i - 1] = g->hands[p][i];
    g->hand_len[p]--;
    return t;
}
static void add_to_hand(game_state_t *g, int p, tile_t t)
{
    if (g->hand_len[p] < 14)
        g->hands[p][g->hand_len[p]++] = t;
}
static void deal_hands(game_state_t *g)
{
    tile_t deck[MAX_TILES];
    int len = 0;
    build_deck(deck, &len);
    shuffle_deck(deck, len);
    int idx = 0;
    for (int p = 0; p < g->nplayers; p++)
    {
        g->hand_len[p] = 7;
        for (int c = 0; c < 7; c++)
            g->hands[p][c] = deck[idx++];
    }
    g->pool_len = 0;
    while (idx < len)
        g->pool[g->pool_len++] = deck[idx++];
    g->train_len = 0;
}
static void choose_opening(game_state_t *g, int *opener_out, tile_t *tile_out)
{
    int opener = -1, best_val = -1, best_is_double = 0, bp = -1, bi = -1;
    for (int p = 0; p < g->nplayers; p++)
    {
        for (int i = 0; i < g->hand_len[p]; i++)
        {
            tile_t t = g->hands[p][i];
            int val = tile_sum(t);
            if (is_double(t))
            {
                if (!best_is_double || t.a > best_val)
                {
                    best_is_double = 1;
                    best_val = t.a;
                    opener = p;
                    bp = p;
                    bi = i;
                    *tile_out = t;
                }
            }
            else if (!best_is_double)
            {
                if (val > best_val)
                {
                    best_val = val;
                    opener = p;
                    bp = p;
                    bi = i;
                    *tile_out = t;
                }
            }
        }
    }
    if (opener < 0)
    {
        opener = 0;
        *tile_out = take_from_hand(g, 0, 0);
    }
    else
    {
        *tile_out = take_from_hand(g, bp, bi);
    }

    g->train[0] = *tile_out;
    g->train_len = 1;
    g->left_end = tile_out->a;
    g->right_end = tile_out->b;
    *opener_out = opener;
    g->turn = (opener + 1) % g->nplayers; // el planificador arrancará aquí
}

/* ===== cola de acciones global ===== */
typedef enum
{
    ACT_PLAY,
    ACT_DRAW,
    ACT_PASS
} act_t;
typedef struct
{
    int table_id, player_id;
    act_t kind;
    int idx_in_hand; // solo PLAY
    int side;        // -1 izq, +1 der (PLAY)
} action_t;

typedef struct
{
    action_t *buf;
    int head, tail, size, capacity;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
} action_queue_t;

static action_queue_t GQ;
static void q_init(action_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->capacity = ACTION_Q_CAP;
    q->buf = malloc(sizeof(action_t) * q->capacity);
    if (!q->buf)
    {
        perror("malloc action queue");
        exit(1);
    }
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}
static void q_grow(action_queue_t *q)
{
    int new_cap = q->capacity * 2;
    action_t *nbuf = malloc(sizeof(action_t) * new_cap);
    if (!nbuf)
    {
        perror("malloc action queue grow");
        exit(1);
    }
    for (int i = 0; i < q->size; i++)
        nbuf[i] = q->buf[(q->head + i) % q->capacity];
    free(q->buf);
    q->buf = nbuf;
    q->capacity = new_cap;
    q->head = 0;
    q->tail = q->size;
}
static void q_push(action_queue_t *q, action_t a)
{
    pthread_mutex_lock(&q->mtx);
    if (q->size == q->capacity)
        q_grow(q);
    q->buf[q->tail] = a;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}
static int q_pop(action_queue_t *q, action_t *out)
{
    int ok = 0;
    pthread_mutex_lock(&q->mtx);
    if (q->size > 0)
    {
        *out = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->size--;
        ok = 1;
    }
    pthread_mutex_unlock(&q->mtx);
    return ok;
}
static void q_destroy(action_queue_t *q)
{
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
}

/* ===== cola de cambios de política / quantum ===== */
typedef struct
{
    int table_id;
    int change_policy;
    policy_t new_policy;
    int change_quantum;
    int new_quantum_ms;
} policy_change_t;

typedef struct
{
    policy_change_t *buf;
    int head, tail, size, capacity;
    int stop;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
} policy_queue_t;

static policy_queue_t POLICY_Q;

static void policy_q_init(policy_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->capacity = 32;
    q->buf = malloc(sizeof(policy_change_t) * q->capacity);
    if (!q->buf)
    {
        perror("malloc policy queue");
        exit(1);
    }
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void policy_q_grow(policy_queue_t *q)
{
    int new_cap = q->capacity * 2;
    policy_change_t *nbuf = malloc(sizeof(policy_change_t) * new_cap);
    if (!nbuf)
    {
        perror("malloc policy queue grow");
        exit(1);
    }
    for (int i = 0; i < q->size; i++)
        nbuf[i] = q->buf[(q->head + i) % q->capacity];
    free(q->buf);
    q->buf = nbuf;
    q->capacity = new_cap;
    q->head = 0;
    q->tail = q->size;
}

static void policy_q_push(policy_queue_t *q, policy_change_t ch)
{
    pthread_mutex_lock(&q->mtx);
    if (q->stop)
    {
        pthread_mutex_unlock(&q->mtx);
        return;
    }
    if (q->size == q->capacity)
        policy_q_grow(q);
    q->buf[q->tail] = ch;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

static int policy_q_try_pop(policy_queue_t *q, policy_change_t *out)
{
    int ret = 0;
    pthread_mutex_lock(&q->mtx);
    if (q->size > 0)
    {
        *out = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->size--;
        ret = 1;
    }
    else if (q->stop)
    {
        ret = -1;
    }
    pthread_mutex_unlock(&q->mtx);
    return ret;
}

static void policy_q_stop(policy_queue_t *q)
{
    pthread_mutex_lock(&q->mtx);
    q->stop = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

static void policy_q_destroy(policy_queue_t *q)
{
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
}

static void request_policy_change(int table_id, policy_t newp)
{
    policy_change_t ch = {
        .table_id = table_id,
        .change_policy = 1,
        .new_policy = newp,
        .change_quantum = 0,
        .new_quantum_ms = 0,
    };
    policy_q_push(&POLICY_Q, ch);
}

static int evaluate_auto_policy(game_state_t *g, policy_t *next_out)
{
    if (g->nplayers <= 0)
        return 0;

    int min_tiles = INT_MAX, max_tiles = 0;
    int min_points = INT_MAX, max_points = 0;

    for (int p = 0; p < g->nplayers; ++p)
    {
        int tiles = g->hand_len[p];
        if (tiles < min_tiles)
            min_tiles = tiles;
        if (tiles > max_tiles)
            max_tiles = tiles;

        int pts = hand_points(g, p);
        if (pts < min_points)
            min_points = pts;
        if (pts > max_points)
            max_points = pts;
    }

    int tile_gap = max_tiles - min_tiles;
    int point_gap = max_points - min_points;

    if (g->pass_streak >= g->nplayers && g->policy != RR)
    {
        *next_out = RR;
        return 1;
    }

    if (tile_gap >= 3 && g->policy != SJF_PLAYERS)
    {
        *next_out = SJF_PLAYERS;
        return 1;
    }

    if (point_gap >= 12 && g->policy != SJF_POINTS)
    {
        *next_out = SJF_POINTS;
        return 1;
    }

    if (g->policy == SJF_POINTS && tile_gap >= 3)
    {
        *next_out = SJF_PLAYERS;
        return 1;
    }

    if (g->policy == SJF_PLAYERS && point_gap >= 12)
    {
        *next_out = SJF_POINTS;
        return 1;
    }

    if (g->policy != FCFS && tile_gap <= 1 && point_gap <= 6 && g->pass_streak == 0)
    {
        *next_out = FCFS;
        return 1;
    }

    if (g->policy == RR && g->pass_streak == 0 && tile_gap >= 3)
    {
        *next_out = SJF_PLAYERS;
        return 1;
    }

    return 0;
}

static int supervisor_apply_policy_change(game_state_t *g, policy_t new_policy, const char *reason)
{
    if (g->policy == new_policy)
        return 0;

    policy_t old = g->policy;
    g->policy = new_policy;
    printf(">> Supervisor%s: Mesa %d cambia política %s -> %s\n",
           reason ? reason : "", g->table_id, policy_name(old), policy_name(new_policy));
    fflush(stdout);
    pthread_cond_broadcast(&g->cv);
    return 1;
}

/* ===== búsqueda de jugada posible ===== */
static int find_play(game_state_t *g, int pid, int *idx_out, int *side_out)
{
    // Primero buscar en extremo izquierdo
    for (int i = 0; i < g->hand_len[pid]; i++)
    {
        tile_t t = g->hands[pid][i];
        if (t.a == g->left_end || t.b == g->left_end)
        {
            *idx_out = i;
            *side_out = -1;
            return 1;
        }
    }
    // Luego en extremo derecho
    for (int i = 0; i < g->hand_len[pid]; i++)
    {
        tile_t t = g->hands[pid][i];
        if (t.a == g->right_end || t.b == g->right_end)
        {
            *idx_out = i;
            *side_out = +1;
            return 1;
        }
    }
    return 0;
}

/* ===== jugadores (productores) ===== */
typedef struct
{
    game_state_t *g;
    int pid;
} player_args_t;

void *player_thread(void *arg)
{
    player_args_t *pa = (player_args_t *)arg;
    game_state_t *g = pa->g;
    int pid = pa->pid;
    free(pa);

    for (;;)
    {
        pthread_mutex_lock(&g->mtx);
        while (!g->finished && g->turn != pid)
            pthread_cond_wait(&g->cv, &g->mtx);
        if (g->finished)
        {
            pthread_mutex_unlock(&g->mtx);
            break;
        }

        // cooldown al inicio del turno planificado
        int cooldown_ms = g->turn_cooldown_ms;
        pthread_mutex_unlock(&g->mtx);
        sleep_ms(cooldown_ms);
        pthread_mutex_lock(&g->mtx);

        if (g->finished)
        {
            pthread_mutex_unlock(&g->mtx);
            break;
        }
        if (g->turn != pid)
        {
            pthread_mutex_unlock(&g->mtx);
            continue;
        }

        // decidir 1 acción
        action_t planned = {.table_id = g->table_id, .player_id = pid};
        int idx = -1, side = 0;
        if (find_play(g, pid, &idx, &side))
        {
            planned.kind = ACT_PLAY;
            planned.idx_in_hand = idx;
            planned.side = side;
        }
        else if (g->pool_len > 0)
        {
            planned.kind = ACT_DRAW;
        }
        else
        {
            planned.kind = ACT_PASS;
        }

        q_push(&GQ, planned);

        // esperar a que el validador aplique (cerrando el "turno planificado")
        while (!g->finished && !g->action_done)
            pthread_cond_wait(&g->cv, &g->mtx);
        pthread_mutex_unlock(&g->mtx);
    }
    return NULL;
}

/* ===== validador (consumidor) ===== */
typedef struct
{
    game_state_t *tables;
    int n_tables;
} validator_args_t;

static int all_tables_finished(game_state_t *t, int n)
{
    for (int i = 0; i < n; i++)
        if (!t[i].finished)
            return 0;
    return 1;
}

static void apply_play(game_state_t *g, int pid, int idx, int side)
{
    tile_t t = take_from_hand(g, pid, idx);
    if (side < 0)
    {
        for (int i = g->train_len; i > 0; i--)
            g->train[i] = g->train[i - 1];
        g->train[0] = t;
        g->train_len++;
        if (t.a == g->left_end)
            g->left_end = t.b;
        else if (t.b == g->left_end)
            g->left_end = t.a;
    }
    else
    {
        g->train[g->train_len++] = t;
        if (t.a == g->right_end)
            g->right_end = t.b;
        else if (t.b == g->right_end)
            g->right_end = t.a;
    }
    g->pass_streak = 0;
    printf("Mesa %d | J%d JUEGA ", g->table_id, pid);
    print_tile(t);
    printf(" en %s -> extremos %d-%d (mano %d)\n", side < 0 ? "izq" : "der", g->left_end, g->right_end, g->hand_len[pid]);
}
static void apply_draw(game_state_t *g, int pid)
{
    if (g->pool_len <= 0)
        return;
    tile_t t = g->pool[--g->pool_len];
    add_to_hand(g, pid, t);
    printf("Mesa %d | J%d ROBA 1. Pozo=%d, Mano=%d\n", g->table_id, pid, g->pool_len, g->hand_len[pid]);
}
static void apply_pass(game_state_t *g, int pid)
{
    g->pass_streak++;
    printf("Mesa %d | J%d PASA. (racha=%d)\n", g->table_id, pid, g->pass_streak);
    if (g->pool_len == 0 && g->pass_streak >= g->nplayers)
    {
        print_points_table(g);
        int win = winner_lowest_points(g);
        printf("=== Mesa %d | CIERRE por bloqueo. Gana J%d ===\n", g->table_id, win);
        g->finished = 1;
    }
}

void *validator_thread(void *arg)
{
    validator_args_t *va = (validator_args_t *)arg;
    game_state_t *tables = va->tables;
    int N = va->n_tables;

    for (;;)
    {
        action_t act;
        int have = 0;
        while (!(have = q_pop(&GQ, &act)))
        {
            if (all_tables_finished(tables, N))
                return NULL;
            sleep_ms(1);
        }
        if (act.table_id < 0 || act.table_id >= N)
            continue;
        game_state_t *g = &tables[act.table_id];

        pthread_mutex_lock(&g->mtx);
        if (g->finished)
        {
            pthread_mutex_unlock(&g->mtx);
            continue;
        }
        if (g->turn != act.player_id)
        {
            pthread_mutex_unlock(&g->mtx);
            continue;
        }

        // aplicar una única acción
        if (act.kind == ACT_PLAY)
        {
            if (act.idx_in_hand >= 0 && act.idx_in_hand < g->hand_len[act.player_id])
            {
                tile_t t = g->hands[act.player_id][act.idx_in_hand];
                int ok = (act.side < 0) ? (t.a == g->left_end || t.b == g->left_end)
                                        : (t.a == g->right_end || t.b == g->right_end);
                if (ok)
                {
                    apply_play(g, act.player_id, act.idx_in_hand, act.side);
                    // pass_streak=0 está dentro de apply_play ✓
                    if (g->hand_len[act.player_id] == 0)
                    {
                        printf("=== Mesa %d | J%d DOMINA. FIN ===\n", g->table_id, act.player_id);
                        g->finished = 1;
                    }
                }
                else
                {
                    // Jugada inválida: no resetear pass_streak aquí
                    if (g->pool_len > 0)
                        apply_draw(g, act.player_id);
                    else
                        apply_pass(g, act.player_id);
                }
            }
        }
        else if (act.kind == ACT_DRAW)
        {
            if (g->pool_len > 0)
                apply_draw(g, act.player_id);
            else
                apply_pass(g, act.player_id);
        }
        else if (act.kind == ACT_PASS)
        {
            apply_pass(g, act.player_id);
        }

        g->steps++;
        if (!g->finished && g->steps >= g->max_steps)
        {
            printf("=== Mesa %d | FIN forzado por límite de pasos ===\n", g->table_id);
            g->finished = 1;
        }

        // marcar fin de "turno planificado" y notificar
        g->action_done = 1;
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mtx);
    }
    return NULL;
}

/* ===== control en caliente (consola) ===== */

void *control_thread(void *arg)
{
    control_args_t *ca = (control_args_t *)arg;

    printf("\n[Supervisor automático] Iniciando monitoreo de mesas...\n");

    while (!all_tables_finished(ca->tables, ca->n_tables))
    {
        for (int i = 0; i < ca->n_tables; ++i)
        {
            game_state_t *g = &ca->tables[i];
            policy_t desired_policy = FCFS;
            int request_change = 0;

            pthread_mutex_lock(&g->mtx);
            if (!g->finished)
            {
                if (evaluate_auto_policy(g, &desired_policy) && desired_policy != g->policy)
                {
                    request_change = 1;
                }
                else if (g->policy == FCFS)
                {
                    desired_policy = (g->nplayers >= 3) ? SJF_PLAYERS : SJF_POINTS;
                    request_change = 1;
                }
            }
            pthread_mutex_unlock(&g->mtx);

            if (request_change)
                request_policy_change(i, desired_policy);

            pthread_mutex_lock(&g->mtx);
            if (!g->finished)
            {
                int desired_cooldown = (g->pass_streak >= g->nplayers) ? 75 : 0;
                if (g->turn_cooldown_ms != desired_cooldown)
                {
                    g->turn_cooldown_ms = desired_cooldown;
                    printf(">> Supervisor auto: Mesa %d ajusta cooldown = %d ms\n", i, desired_cooldown);
                    pthread_cond_broadcast(&g->cv);
                }

                int desired_quantum = (g->policy == RR) ? 120 : 200;
                if (g->rr_quantum_ms != desired_quantum)
                {
                    g->rr_quantum_ms = desired_quantum;
                    printf(">> Supervisor auto: Mesa %d ajusta quantum = %d ms\n", i, desired_quantum);
                }
            }
            pthread_mutex_unlock(&g->mtx);
        }

        sleep_ms(100);
    }

    printf("[Supervisor automático] Finalizó el monitoreo: todas las mesas terminaron.\n");
    return NULL;
}

typedef struct
{
    game_state_t *tables;
    int n_tables;
} policy_supervisor_args_t;

void *policy_supervisor_thread(void *arg)
{
    policy_supervisor_args_t *psa = (policy_supervisor_args_t *)arg;

    int stop_requested = 0;
    for (;;)
    {
        policy_change_t change;
        int popped = policy_q_try_pop(&POLICY_Q, &change);
        if (popped == 1)
        {
            if (change.table_id < 0 || change.table_id >= psa->n_tables)
                continue;

            game_state_t *g = &psa->tables[change.table_id];
            pthread_mutex_lock(&g->mtx);
            if (!g->finished)
            {
                if (change.change_policy)
                    supervisor_apply_policy_change(g, change.new_policy, " (solicitado)");
            }
            pthread_mutex_unlock(&g->mtx);
            continue;
        }
        if (popped == -1)
            stop_requested = 1;
        if (stop_requested && all_tables_finished(psa->tables, psa->n_tables))
            break;

        sleep_ms(50);
    }

    return NULL;
}

/* ===== planificador por mesa ===== */
static int pick_next_fcfs(game_state_t *g, int current)
{
    return (current + 1) % g->nplayers;
}
static int pick_next_rr(game_state_t *g, int current)
{
    // por ahora 1 acción por turno => igual a FCFS; luego podemos permitir N acciones por quantum
    (void)g;
    return (current + 1) % g->nplayers;
}
static int pick_next_sjf_points(game_state_t *g)
{
    int best = -1, best_pts = INT_MAX, best_tiles = INT_MAX;
    for (int p = 0; p < g->nplayers; p++)
    {
        int pts = hand_points(g, p), tiles = g->hand_len[p];
        if (pts < best_pts || (pts == best_pts && tiles < best_tiles) || (pts == best_pts && tiles == best_tiles && p < best))
        {
            best = p;
            best_pts = pts;
            best_tiles = tiles;
        }
    }
    return best;
}
static int pick_next_sjf_players(game_state_t *g)
{
    int best = -1, best_tiles = INT_MAX;
    for (int p = 0; p < g->nplayers; p++)
    {
        int tiles = g->hand_len[p];
        if (tiles < best_tiles || (tiles == best_tiles && p < best))
        {
            best = p;
            best_tiles = tiles;
        }
    }
    return best;
}
static int pick_next_player(game_state_t *g, int current)
{
    switch (g->policy)
    {
    case FCFS:
        return pick_next_fcfs(g, current);
    case RR:
        return pick_next_rr(g, current);
    case SJF_POINTS:
        return pick_next_sjf_points(g);
    case SJF_PLAYERS:
        return pick_next_sjf_players(g);
    }
    return (current + 1) % g->nplayers;
}

void *scheduler_thread(void *arg)
{
    game_state_t *g = (game_state_t *)arg;

    pthread_mutex_lock(&g->mtx);
    int current = g->turn; // ya viene inicializado por choose_opening
    pthread_mutex_unlock(&g->mtx);

    for (;;)
    {
        pthread_mutex_lock(&g->mtx);
        if (g->finished)
        {
            pthread_mutex_unlock(&g->mtx);
            break;
        }

        // programar al 'current': despertar jugadores
        g->action_done = 0;
        // g->turn ya apunta a current
        pthread_cond_broadcast(&g->cv);

        // esperar a que el validador aplique UNA acción
        while (!g->finished && !g->action_done)
            pthread_cond_wait(&g->cv, &g->mtx);
        if (g->finished)
        {
            pthread_mutex_unlock(&g->mtx);
            break;
        }

        // decidir siguiente según política
        int next = pick_next_player(g, current);
        g->turn = next;
        current = next;

        pthread_mutex_unlock(&g->mtx);
    }
    return NULL;
}

/* ===== mesa ===== */
static void init_table(game_state_t *g, int table_id, int nplayers, policy_t pol)
{
    memset(g, 0, sizeof(*g));
    g->table_id = table_id;
    g->nplayers = nplayers;
    g->max_steps = 800;
    g->steps = 0;
    g->pass_streak = 0;
    g->policy = pol;
    g->rr_quantum_ms = 200; // reservado (futuro)
    g->turn_cooldown_ms = DEFAULT_TURN_COOLDOWN_MS;
    g->action_done = 0;
    pthread_mutex_init(&g->mtx, NULL);
    pthread_cond_init(&g->cv, NULL);
}

void *table_thread(void *arg)
{
    game_state_t *g = (game_state_t *)arg;

    deal_hands(g);
    int opener = -1;
    tile_t first;
    choose_opening(g, &opener, &first);

    pthread_mutex_lock(&g->mtx);
    printf("\n=== Mesa %d: %d jugadores — Política: %s ===\n", g->table_id, g->nplayers,
           policy_name(g->policy));
    printf("Apertura: Jugador %d juega ", opener);
    print_tile(first);
    printf("  -> extremos: %d y %d\n", g->left_end, g->right_end);
    for (int p = 0; p < g->nplayers; p++)
    {
        printf("Mano J%d (%2d fichas): ", p, g->hand_len[p]);
        for (int i = 0; i < g->hand_len[p]; i++)
        {
            print_tile(g->hands[p][i]);
            printf(" ");
        }
        printf("\n");
    }
    printf("Pozo: %d fichas\n", g->pool_len);
    pthread_mutex_unlock(&g->mtx);

    // jugadores
    pthread_t th_players[MAX_PLAYERS];
    for (int p = 0; p < g->nplayers; p++)
    {
        player_args_t *pa = malloc(sizeof(*pa));
        pa->g = g;
        pa->pid = p;
        if (pthread_create(&th_players[p], NULL, player_thread, pa) != 0)
        {
            perror("pthread_create(player)");
            exit(1);
        }
    }

    // planificador por mesa
    pthread_t th_sched;
    if (pthread_create(&th_sched, NULL, scheduler_thread, g) != 0)
    {
        perror("pthread_create(scheduler)");
        exit(1);
    }

    // esperar fin de mesa
    pthread_mutex_lock(&g->mtx);
    while (!g->finished)
        pthread_cond_wait(&g->cv, &g->mtx);
    pthread_mutex_unlock(&g->mtx);
    pthread_cond_broadcast(&g->cv);

    pthread_join(th_sched, NULL);
    for (int p = 0; p < g->nplayers; p++)
        pthread_join(th_players[p], NULL);
    printf("=== Mesa %d: terminó ===\n", g->table_id);
    return NULL;
}

/* ===== main ===== */
typedef struct
{
    game_state_t *tables;
    int n_tables;
} validator_args_2; // alias para claridad

void *validator_thread(void *); // fwd

int main(void)
{
    srand((unsigned)time(NULL));
    setvbuf(stdout, NULL, _IONBF, 0);
    int n_tables = 0;
    char input_buf[32];

    printf("¿Cuántas mesas quieres crear? ");
    fflush(stdout);
    if (!fgets(input_buf, sizeof(input_buf), stdin) || sscanf(input_buf, "%d", &n_tables) != 1 || n_tables <= 0)
    {
        puts("Valor inválido.");
        return 1;
    }

    // Puedes cambiar la política por defecto aquí:
    policy_t default_policy = SJF_POINTS; // FCFS | SJF_POINTS | SJF_PLAYERS | RR

    game_state_t *tables = calloc(n_tables, sizeof(game_state_t));
    pthread_t *th_tables = calloc(n_tables, sizeof(pthread_t));
    if (!tables || !th_tables)
    {
        perror("alloc");
        return 1;
    }

    // Validador único global
    q_init(&GQ);
    policy_q_init(&POLICY_Q);
    validator_args_t va = {.tables = tables, .n_tables = n_tables};
    pthread_t th_validator;
    if (pthread_create(&th_validator, NULL, validator_thread, &va) != 0)
    {
        perror("pthread_create(validator)");
        return 1;
    }

    policy_supervisor_args_t psa = {.tables = tables, .n_tables = n_tables};
    pthread_t th_policy_supervisor;
    if (pthread_create(&th_policy_supervisor, NULL, policy_supervisor_thread, &psa) != 0)
    {
        perror("pthread_create(policy_supervisor)");
        return 1;
    }

    // Hilo de control en caliente (consola)
    control_args_t ca = {.tables = tables, .n_tables = n_tables};
    pthread_t th_control;
    int control_thread_started = 0;
    if (pthread_create(&th_control, NULL, control_thread, &ca) != 0)
    {
        perror("pthread_create(control)"); /* no abortamos; solo avisamos */
    }
    else
    {
        control_thread_started = 1;
    }

    // Lanzar mesas con la política elegida
    for (int i = 0; i < n_tables; i++)
    {
        int np = 2 + rand() % 3;
        init_table(&tables[i], i, np, default_policy);
        if (pthread_create(&th_tables[i], NULL, table_thread, &tables[i]) != 0)
        {
            perror("pthread_create(table)");
            return 1;
        }
    }

    for (int i = 0; i < n_tables; i++)
        pthread_join(th_tables[i], NULL);
    pthread_join(th_validator, NULL);
    if (control_thread_started)
        pthread_join(th_control, NULL);

    policy_q_stop(&POLICY_Q);
    pthread_join(th_policy_supervisor, NULL);

    q_destroy(&GQ);
    policy_q_destroy(&POLICY_Q);
    puts("\nTodas las mesas han terminado.");
    free(th_tables);
    free(tables);
    return 0;
}
