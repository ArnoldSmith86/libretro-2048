#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include "game_shared.h"

static game_t game;

/* Score animations */
static int delta_score;
static float delta_score_time;
static float frame_time = 0.016;

#define PI 3.14159

/* out back bicubic
 * from http://www.timotheegroleau.com/Flash/experiments/easing_function_generator.htm
 */
float bump_out(float v0, float v1, float t)
{
   float ts, tc;

   t /= 1; /* intensity (d) */

   ts = t  * t;
   tc = ts * t;
   return v0 + v1 * (4*tc + -9*ts + 6*t);
}

/* interpolation functions */
float lerp(float v0, float v1, float t)
{
   return v0 * (1 - t) + v1 * t;
}

float cos_interp(float v0,float v1, float t)
{
   float t2;

   t2 = (1-cos(t*PI))/2;
   return(v0*(1-t2)+v1*t2);
}

void *game_data(void)
{
   return &game;
}

void *game_save_data(void)
{
   int row, col;

   /* stop animations */
   for (row = 0; row < 4; row++)
   {
      for (col = 0; col < 4; col++)
      {
         game.grid[row * 4 + col].appear_time = 1;
         game.grid[row * 4 + col].move_time   = 1;
      }
   }

   delta_score_time = 1;

   /* show title screen when the game gets loaded again, but preserve an
    * active game (PLAYING, PAUSED) and a pending name entry (NAME_ENTRY). */
   if (game.state != STATE_PLAYING  &&
       game.state != STATE_PAUSED   &&
       game.state != STATE_NAME_ENTRY)
   {
      game.score = 0;
      game.state = STATE_TITLE;
   }

   return &game;
}

unsigned game_data_size(void)
{
   return sizeof(game);
}

void render_game(void)
{
   if (game.state == STATE_PLAYING)
      render_playing();
   else if (game.state == STATE_TITLE)
      render_title();
   else if (game.state == STATE_GAME_OVER || game.state == STATE_WON)
      render_win_or_game_over();
   else if (game.state == STATE_PAUSED)
      render_paused();
   else if (game.state == STATE_NAME_ENTRY)
      render_name_entry();
   else if (game.state == STATE_HIGHSCORES)
      render_highscores();
}

static void add_tile(void)
{
   int i, j;
   cell_t *empty[GRID_SIZE];

   if (game.state != STATE_PLAYING)
      return;

   j = 0;
   for (i = 0; i < GRID_SIZE; i++)
   {
      empty[j] = NULL;
      if (!game.grid[i].value)
         empty[j++] = &game.grid[i];
   }

   if (j)
   {
      j = rand() % j;
      empty[j]->old_pos = empty[j]->pos;
      empty[j]->source = NULL;
      empty[j]->move_time = 1;
      empty[j]->appear_time = 0;
      empty[j]->value = ((float)rand() / RAND_MAX) < 0.9 ? 1 : 2;
   }
   else
      change_state(STATE_NAME_ENTRY);
}

void init_game(void)
{
   memset(&game, 0, sizeof(game));

   game.state = STATE_TITLE;
}

/* Called after SRAM is loaded to clamp any fields that could be corrupt
 * due to a save from an older struct layout. */
void game_validate(void)
{
   int i;

   /* State must be one of the known values; anything else → title */
   if (game.state < STATE_TITLE || game.state > STATE_HIGHSCORES)
      game.state = STATE_TITLE;
   /* Don't restore highscores screen across sessions */
   if (game.state == STATE_HIGHSCORES)
      game.state = STATE_TITLE;

   /* Clamp player count */
   if (game.player_count < 0 || game.player_count > MAX_PLAYERS)
      game.player_count = 0;

   /* Clamp per-player score counts */
   for (i = 0; i < game.player_count; i++)
   {
      if (game.players[i].score_count < 0 ||
          game.players[i].score_count > MAX_SCORES_PER_PLAYER)
         game.players[i].score_count = 0;
      if (game.players[i].month_score_count < 0 ||
          game.players[i].month_score_count > MAX_SCORES_PER_PLAYER)
         game.players[i].month_score_count = 0;
   }
}

void start_game(void)
{
   int row, col;
   game.score = 0;

   for (row = 0; row < 4; row++)
   {
      for (col = 0; col < 4; col++)
      {
         cell_t *cell = &game.grid[row * 4 + col];

         cell->pos.x = col;
         cell->pos.y = row;
         cell->old_pos = cell->pos;
         cell->move_time = 1;
         cell->appear_time = 0;
         cell->value = 0;
         cell->source = NULL;
      }
   }

   game.won_before = false;
   game.suspended  = false;

   /* reset +score animation */
   delta_score      = 0;
   delta_score_time = 1;

   add_tile();
   add_tile();
}

static bool cells_available(void)
{
   int row, col;
   for (row = 0; row < GRID_HEIGHT; row++)
   {
      for (col = 0; col < GRID_WIDTH; col++)
      {
         if (!game.grid[row * GRID_WIDTH + col].value)
            return true;
      }
   }

   return false;
}

static bool matches_available(void)
{
   int row, col;
   for (row = 0; row < GRID_HEIGHT; row++)
   {
      for (col = 0; col < GRID_WIDTH; col++)
      {
         cell_t *cell = &game.grid[row * GRID_WIDTH + col];

         if (!cell->value)
            continue;

         if ((col > 0 && game.grid[row * GRID_WIDTH + col - 1].value == cell->value) ||
             (col < GRID_WIDTH - 1 && game.grid[row * GRID_WIDTH + col + 1].value == cell->value) ||
             (row > 0 && game.grid[(row - 1) * GRID_WIDTH + col].value == cell->value) ||
             (row < GRID_HEIGHT - 1 && game.grid[(row + 1) * GRID_WIDTH + col].value == cell->value))
            return true;
      }
   }

   return false;
}

static bool move_tiles(void)
{
   int row, col;
   int vec_x, vec_y;
   int col_begin, col_end, col_inc;
   int row_begin, row_end, row_inc;
   bool moved = false;

   switch (game.direction)
   {
      case DIR_UP:
         vec_x = 0; vec_y = -1;
         break;
      case DIR_DOWN:
         vec_x = 0; vec_y = 1;
         break;
      case DIR_RIGHT:
         vec_x = 1; vec_y = 0;
         break;
      case DIR_LEFT:
         vec_x = -1; vec_y = 0;
         break;
      default:
         return false;
   }

   col_begin = 0;
   col_end   = 4;
   col_inc   = 1;
   row_begin = 0;
   row_end   = 4;
   row_inc   = 1;

   if (vec_x > 0) {
      col_begin = 3;
      col_end   = -1;
      col_inc   = -1;
   }

   if (vec_y > 0)
   {
      row_begin = 3;
      row_end   = -1;
      row_inc   = -1;
   }

   delta_score = game.score;

   /* clear source cell and save current position in the grid */
   for (row = row_begin; row != row_end; row += row_inc)
   {
      for (col = col_begin; col != col_end; col += col_inc)
      {
         cell_t *cell = &game.grid[row * 4 + col];
         cell->old_pos = cell->pos;
         cell->source = NULL;
         cell->move_time = 1;
         cell->appear_time = 1;
      }
   }

   for (row = row_begin; row != row_end; row += row_inc)
   {
      for (col = col_begin; col != col_end; col += col_inc)
      {
         int new_row, new_col;
         cell_t *farthest, *next;
         cell_t *cell = &game.grid[row * 4 + col];

         if (!cell->value)
            continue;

         next    = cell;
         new_row = row;
         new_col = col;

         do
         {
            farthest = next;

            new_row += vec_y;
            new_col += vec_x;

            if (new_row < 0 || new_col < 0 || new_row > 3 || new_col > 3)
               break;

            next = &game.grid[new_row * 4 + new_col];
         } while (!next->value);

         /* only tiles that have not been merged */
         if (next->value && next->value == cell->value && next != cell && !next->source)
         {
            next->value = cell->value + 1;
            next->source = cell;
            next->old_pos = cell->pos;
            next->move_time = 0;
            cell->value = 0;

            game.score += 2 << next->value;
            moved = true;

            if (next->value == 11 && !game.won_before)
            {
               game.won_before = true;
               change_state(STATE_WON);
            }
         }
         else if (farthest != cell)
         {
            farthest->value = cell->value;
            farthest->old_pos = cell->pos;
            farthest->move_time = 0;
            cell->value = 0;
            moved = true;
         }
      }
   }

   delta_score      = game.score - delta_score;
   delta_score_time = delta_score == 0 ? 1 : 0;

   return moved;
}

void game_update(float delta, key_state_t *new_ks)
{
   frame_time = delta;

   handle_input(new_ks);

   if (game.state == STATE_PLAYING)
   {
      if (game.direction != DIR_NONE && move_tiles())
         add_tile();

      if (!matches_available() && !cells_available())
         change_state(STATE_NAME_ENTRY);
   }
}

float *game_get_frame_time(void)
{
   return &frame_time;
}

int *game_get_delta_score(void)
{
   return &delta_score;
}

float *game_get_delta_score_time(void)
{
   return &delta_score_time;
}

int game_get_score(void)
{
   return game.score;
}

int game_get_best_score(void)
{
   return game.best_score;
}
cell_t * game_get_grid(void)
{
   return game.grid;
}

player_record_t *game_get_players(void)
{
   return game.players;
}

int game_get_player_count(void)
{
   return game.player_count;
}

int game_get_hs_page(void)
{
   return game.hs_page;
}

const char *game_get_name_entry(void)
{
   return game.name_entry;
}

int game_get_name_cursor(void)
{
   return game.name_cursor;
}

bool game_get_suspended(void)
{
   return game.suspended;
}

int game_get_hs_time_filter(void)
{
   return game.hs_time_filter;
}

int game_get_hs_row_focus(void)
{
   return game.hs_row_focus;
}

static void fill_entry(highscore_entry_t *entry, int score,
                       struct tm *tm_info, int max_tile)
{
   entry->score     = score;
   entry->best_tile = (uint8_t)max_tile;
   if (tm_info)
   {
      entry->year  = (int16_t)(tm_info->tm_year + 1900);
      entry->month = (uint8_t)(tm_info->tm_mon + 1);
      entry->day   = (uint8_t)tm_info->tm_mday;
   }
   else
   {
      entry->year  = 0;
      entry->month = 0;
      entry->day   = 0;
   }
}

static void add_highscore(const char *name, int score)
{
   player_record_t   *player = NULL;
   highscore_entry_t *entry  = NULL;
   time_t t;
   struct tm *tm_info;
   int i, gi, min_idx, max_tile;
   int16_t cur_year  = 0;
   uint8_t cur_month = 0;

   /* Find or create player record */
   for (i = 0; i < game.player_count; i++)
      if (strncmp(game.players[i].name, name, 3) == 0)
      { player = &game.players[i]; break; }

   if (!player)
   {
      if (game.player_count >= MAX_PLAYERS)
         return;
      player = &game.players[game.player_count++];
      strncpy(player->name, name, 3);
      player->name[3]         = '\0';
      player->score_count     = 0;
      player->month_score_count = 0;
      player->month_scores_year  = 0;
      player->month_scores_month = 0;
   }

   t       = time(NULL);
   tm_info = localtime(&t);
   if (tm_info)
   {
      cur_year  = (int16_t)(tm_info->tm_year + 1900);
      cur_month = (uint8_t)(tm_info->tm_mon + 1);
   }

   max_tile = 0;
   for (gi = 0; gi < GRID_SIZE; gi++)
      if (game.grid[gi].value > max_tile)
         max_tile = game.grid[gi].value;

   strncpy(game.last_name, name, 4);

   /* --- All-time personal list --- */
   if (player->score_count < MAX_SCORES_PER_PLAYER)
   {
      entry = &player->scores[player->score_count++];
   }
   else
   {
      /* Replace their lowest score if this one is better */
      min_idx = 0;
      for (i = 1; i < player->score_count; i++)
         if (player->scores[i].score < player->scores[min_idx].score)
            min_idx = i;
      if (score > player->scores[min_idx].score)
         entry = &player->scores[min_idx];
   }
   if (entry)
      fill_entry(entry, score, tm_info, max_tile);

   /* --- Monthly personal list --- */
   /* Reset list if the stored month doesn't match current month */
   if (player->month_scores_year  != cur_year ||
       player->month_scores_month != cur_month)
   {
      player->month_score_count  = 0;
      player->month_scores_year  = cur_year;
      player->month_scores_month = cur_month;
   }

   entry = NULL;
   if (player->month_score_count < MAX_SCORES_PER_PLAYER)
   {
      entry = &player->month_scores[player->month_score_count++];
   }
   else
   {
      /* Replace lowest monthly score if this one is better */
      min_idx = 0;
      for (i = 1; i < player->month_score_count; i++)
         if (player->month_scores[i].score < player->month_scores[min_idx].score)
            min_idx = i;
      if (score > player->month_scores[min_idx].score)
         entry = &player->month_scores[min_idx];
   }
   if (entry)
      fill_entry(entry, score, tm_info, max_tile);
}

static int count_hs_players(void)
{
   return game.player_count;
}

game_state_t game_get_state(void)
{
   return game.state;
}

static void end_game(void)
{
   game.best_score = game.score > game.best_score ? game.score : game.best_score;
}

void change_state(game_state_t state)
{
   switch (game.state)
   {
      case STATE_TITLE:
         assert(state == STATE_PLAYING || state == STATE_HIGHSCORES);
         if (state == STATE_HIGHSCORES)
         {
            game.hs_page        = 0;
            game.hs_row_focus   = 0;
            game.hs_time_filter = 0;
         }
         if (state == STATE_PLAYING)
         {
            game.state = state;
            if (game.suspended)
               game.suspended = false; /* resume in-place, grid/score untouched */
            else
               start_game();
            return;
         }
         break;

      case STATE_GAME_OVER:
         assert(state == STATE_PLAYING || state == STATE_HIGHSCORES);
         if (state == STATE_HIGHSCORES)
         {
            game.hs_page        = 0;
            game.hs_row_focus   = 0;
            game.hs_time_filter = 0;
         }
         if (state == STATE_PLAYING)
         {
            game.state = state;
            start_game();
            return;
         }
         break;

      case STATE_PLAYING:
         assert(state == STATE_NAME_ENTRY || state == STATE_WON || state == STATE_PAUSED);
         if (state == STATE_WON)
            end_game();
         if (state == STATE_NAME_ENTRY)
         {
            if (game.last_name[0] >= 'A' && game.last_name[0] <= 'Z')
               strncpy(game.name_entry, game.last_name, 4);
            else
            {
               game.name_entry[0] = game.name_entry[1] = game.name_entry[2] = 'A';
               game.name_entry[3] = '\0';
            }
            game.name_cursor = 0;
         }
         break;

      case STATE_NAME_ENTRY:
         assert(state == STATE_GAME_OVER);
         end_game();
         break;

      case STATE_WON:
         end_game();
         if (state == STATE_TITLE)
            game.suspended = false; /* won game → fresh start from title */
         assert(state == STATE_TITLE || state == STATE_PLAYING);
         break;

      case STATE_PAUSED:
         assert(state == STATE_PLAYING || state == STATE_TITLE);
         if (state == STATE_TITLE)
         {
            end_game();
            game.suspended = true; /* mid-game suspend: title START will resume */
         }
         break;

      case STATE_HIGHSCORES:
         assert(state == STATE_TITLE);
         break;
   }

   game.state = state;
}

void handle_input(key_state_t *ks)
{
   game.direction = DIR_NONE;

   if (game.state == STATE_TITLE || game.state == STATE_GAME_OVER || game.state == STATE_WON)
   {
      if (!ks->start && game.old_ks.start)
         change_state(game.state == STATE_WON ? STATE_TITLE : STATE_PLAYING);
      else if (game.state == STATE_WON && !ks->select && game.old_ks.select)
      {
         change_state(STATE_PLAYING);
         add_tile();
      }
      else if ((game.state == STATE_TITLE || game.state == STATE_GAME_OVER) &&
               !ks->select && game.old_ks.select)
         change_state(STATE_HIGHSCORES);
   }
   else if (game.state == STATE_NAME_ENTRY)
   {
      if (ks->up && !game.old_ks.up)
         game.name_entry[game.name_cursor] =
               (game.name_entry[game.name_cursor] - 'A' + 1) % 26 + 'A';
      if (ks->down && !game.old_ks.down)
         game.name_entry[game.name_cursor] =
               (game.name_entry[game.name_cursor] - 'A' + 25) % 26 + 'A';
      if (ks->left && !game.old_ks.left && game.name_cursor > 0)
         game.name_cursor--;
      if (ks->right && !game.old_ks.right && game.name_cursor < 2)
         game.name_cursor++;
      if (ks->start && !game.old_ks.start)
      {
         add_highscore(game.name_entry, game.score);
         change_state(STATE_GAME_OVER);
      }
      if (!ks->select && game.old_ks.select)
      {
         /* Cycle through known player names */
         int count = game.player_count;
         if (count > 0)
         {
            int cur_idx = -1;
            int j;
            for (j = 0; j < count; j++)
               if (strncmp(game.players[j].name, game.name_entry, 3) == 0)
               { cur_idx = j; break; }
            cur_idx = (cur_idx + 1) % count;
            strncpy(game.name_entry, game.players[cur_idx].name, 3);
            game.name_entry[3] = '\0';
         }
      }
   }
   else if (game.state == STATE_HIGHSCORES)
   {
      if (ks->up && !game.old_ks.up)
         game.hs_row_focus = 0;
      if (ks->down && !game.old_ks.down)
         game.hs_row_focus = 1;

      if (game.hs_row_focus == 0)
      {
         /* left/right navigates player pages */
         if (ks->left && !game.old_ks.left)
         {
            int num_pages = count_hs_players() + 1;
            game.hs_page  = (game.hs_page + num_pages - 1) % num_pages;
         }
         else if (ks->right && !game.old_ks.right)
         {
            int num_pages = count_hs_players() + 1;
            game.hs_page  = (game.hs_page + 1) % num_pages;
         }
      }
      else
      {
         /* left/right cycles time filter */
         if ((ks->left && !game.old_ks.left) || (ks->right && !game.old_ks.right))
            game.hs_time_filter = !game.hs_time_filter;
      }

      if ((!ks->start && game.old_ks.start) || (!ks->select && game.old_ks.select))
         change_state(STATE_TITLE);
   }
   else if (game.state == STATE_PLAYING)
   {
      if (ks->up && !game.old_ks.up)
         game.direction = DIR_UP;
      else if (ks->right && !game.old_ks.right)
         game.direction = DIR_RIGHT;
      else if (ks->down && !game.old_ks.down)
         game.direction = DIR_DOWN;
      else if (ks->left && !game.old_ks.left)
         game.direction = DIR_LEFT;
      if (ks->start && !game.old_ks.start)
         change_state(STATE_PAUSED);
   }
   else if (game.state == STATE_PAUSED)
   {
      if (ks->start && !game.old_ks.start)
         change_state(STATE_PLAYING);
      else if (!ks->select && game.old_ks.select)
         change_state(STATE_TITLE);
   }

   game.old_ks = *ks;
}

void game_reset(void)
{
   start_game();
}

void grid_to_screen(vector_t pos, int *x, int *y)
{
   *x = SPACING * 2 + ((TILE_SIZE + SPACING) * pos.x);
   *y = BOARD_OFFSET_Y + SPACING + ((TILE_SIZE + SPACING) * pos.y);
}
