/* copyright 2013 Sascha Kruse and contributors (see LICENSE for licensing information) */

/**
 * @file queues.c
 * @brief All important functions to handle the notification queues for
 * history, entrance and currently displayed ones.
 *
 * Every method requires to have executed queues_init() at the start.
 *
 * A read only representation of the queue with the current notifications
 * can get acquired by calling queues_get_displayed().
 *
 * When ending the program or resetting the queues, tear down the stack with
 * queues_teardown(). (And reinit with queues_init() if needed.)
 */
#include "queues.h"

#include <assert.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "notification.h"
#include "settings.h"
#include "utils.h"

/* notification lists */
static GQueue *waiting   = NULL; /**< all new notifications get into here */
static GQueue *displayed = NULL; /**< currently displayed notifications */
static GQueue *history   = NULL; /**< history of displayed notifications */

unsigned int displayed_limit = 0;
int next_notification_id = 1;
bool pause_displayed = false;

static bool queues_stack_duplicate(notification *n);

/* see queues.h */
void queues_init(void)
{
        history   = g_queue_new();
        displayed = g_queue_new();
        waiting   = g_queue_new();
}

/* see queues.h */
void queues_displayed_limit(unsigned int limit)
{
        displayed_limit = limit;
}

/* see queues.h */
const GList *queues_get_displayed(void)
{
        return g_queue_peek_head_link(displayed);
}

/* see queues.h */
unsigned int queues_length_waiting(void)
{
        return waiting->length;
}

/* see queues.h */
unsigned int queues_length_displayed(void)
{
        return displayed->length;
}

/* see queues.h */
unsigned int queues_length_history(void)
{
        return history->length;
}

/* see queues.h */
int queues_notification_insert(notification *n)
{

        /* do not display the message, if the message is empty */
        if (strlen(n->msg) == 0) {
                if (settings.always_run_script) {
                        notification_run_script(n);
                }
                LOG_M("Skipping notification: '%s' '%s'", n->body, n->summary);
                return 0;
        }
        /* Do not insert the message if it's a command */
        if (strcmp("DUNST_COMMAND_PAUSE", n->summary) == 0) {
                pause_displayed = true;
                return 0;
        }
        if (strcmp("DUNST_COMMAND_RESUME", n->summary) == 0) {
                pause_displayed = false;
                return 0;
        }
        if (strcmp("DUNST_COMMAND_TOGGLE", n->summary) == 0) {
                pause_displayed = !pause_displayed;
                return 0;
        }

        if (n->id == 0) {
                n->id = ++next_notification_id;
                if (!settings.stack_duplicates || !queues_stack_duplicate(n))
                        g_queue_insert_sorted(waiting, n, notification_cmp_data, NULL);
        } else {
                if (!queues_notification_replace_id(n))
                        g_queue_insert_sorted(waiting, n, notification_cmp_data, NULL);
        }

        if (settings.print_notifications)
                notification_print(n);

        return n->id;
}

/**
 * Replaces duplicate notification and stacks it
 *
 * @return true, if notification got stacked
 * @return false, if notification did not get stacked
 */
static bool queues_stack_duplicate(notification *n)
{
        for (GList *iter = g_queue_peek_head_link(displayed); iter;
             iter = iter->next) {
                notification *orig = iter->data;
                if (notification_is_duplicate(orig, n)) {
                        /* If the progress differs, probably notify-send was used to update the notification
                         * So only count it as a duplicate, if the progress was not the same.
                         * */
                        if (orig->progress == n->progress) {
                                orig->dup_count++;
                        } else {
                                orig->progress = n->progress;
                        }

                        iter->data = n;

                        n->start = time_monotonic_now();

                        n->dup_count = orig->dup_count;

                        signal_notification_closed(orig, 1);

                        notification_free(orig);
                        return true;
                }
        }

        for (GList *iter = g_queue_peek_head_link(waiting); iter;
             iter = iter->next) {
                notification *orig = iter->data;
                if (notification_is_duplicate(orig, n)) {
                        /* If the progress differs, probably notify-send was used to update the notification
                         * So only count it as a duplicate, if the progress was not the same.
                         * */
                        if (orig->progress == n->progress) {
                                orig->dup_count++;
                        } else {
                                orig->progress = n->progress;
                        }
                        iter->data = n;

                        n->dup_count = orig->dup_count;

                        signal_notification_closed(orig, 1);

                        notification_free(orig);
                        return true;
                }
        }

        return false;
}

/* see queues.h */
bool queues_notification_replace_id(notification *new)
{

        for (GList *iter = g_queue_peek_head_link(displayed);
                    iter;
                    iter = iter->next) {
                notification *old = iter->data;
                if (old->id == new->id) {
                        iter->data = new;
                        new->start = time_monotonic_now();
                        new->dup_count = old->dup_count;
                        notification_run_script(new);
                        notification_free(old);
                        return true;
                }
        }

        for (GList *iter = g_queue_peek_head_link(waiting);
                    iter;
                    iter = iter->next) {
                notification *old = iter->data;
                if (old->id == new->id) {
                        iter->data = new;
                        new->dup_count = old->dup_count;
                        notification_free(old);
                        return true;
                }
        }
        return false;
}

/* see queues.h */
void queues_notification_close_id(int id, enum reason reason)
{
        notification *target = NULL;

        for (GList *iter = g_queue_peek_head_link(displayed); iter;
             iter = iter->next) {
                notification *n = iter->data;
                if (n->id == id) {
                        g_queue_remove(displayed, n);
                        target = n;
                        break;
                }
        }

        for (GList *iter = g_queue_peek_head_link(waiting); iter;
             iter = iter->next) {
                notification *n = iter->data;
                if (n->id == id) {
                        assert(target == NULL);
                        g_queue_remove(waiting, n);
                        target = n;
                        break;
                }
        }

        if (target) {
                //Don't notify clients if notification was pulled from history
                if (!target->redisplayed)
                        signal_notification_closed(target, reason);
                queues_history_push(target);
        }
}

/* see queues.h */
void queues_notification_close(notification *n, enum reason reason)
{
        assert(n != NULL);
        queues_notification_close_id(n->id, reason);
}

/* see queues.h */
void queues_history_pop(void)
{
        if (g_queue_is_empty(history))
                return;

        notification *n = g_queue_pop_tail(history);
        n->redisplayed = true;
        n->start = 0;
        n->timeout = settings.sticky_history ? 0 : n->timeout;
        g_queue_push_head(waiting, n);
}

/* see queues.h */
void queues_history_push(notification *n)
{
        if (!n->history_ignore) {
                if (settings.history_length > 0 && history->length >= settings.history_length) {
                        notification *to_free = g_queue_pop_head(history);
                        notification_free(to_free);
                }

                g_queue_push_tail(history, n);
        } else {
                notification_free(n);
        }
}

/* see queues.h */
void queues_history_push_all(void)
{
        while (displayed->length > 0) {
                queues_notification_close(g_queue_peek_head_link(displayed)->data, REASON_USER);
        }

        while (waiting->length > 0) {
                queues_notification_close(g_queue_peek_head_link(waiting)->data, REASON_USER);
        }
}

/* see queues.h */
void queues_check_timeouts(bool idle, bool fullscreen)
{
        /* nothing to do */
        if (displayed->length == 0)
                return;

        bool is_idle = fullscreen ? false : idle;

        GList *iter = g_queue_peek_head_link(displayed);
        while (iter) {
                notification *n = iter->data;

                /*
                 * Update iter to the next item before we either exit the
                 * current iteration of the loop or potentially delete the
                 * notification which would invalidate the pointer.
                 */
                iter = iter->next;

                /* don't timeout when user is idle */
                if (is_idle && !n->transient) {
                        n->start = time_monotonic_now();
                        continue;
                }

                /* skip hidden and sticky messages */
                if (n->start == 0 || n->timeout == 0) {
                        continue;
                }

                /* remove old message */
                if (time_monotonic_now() - n->start > n->timeout) {
                        queues_notification_close(n, REASON_TIME);
                }
        }
}

/* see queues.h */
void queues_update(bool fullscreen)
{
        if (pause_displayed) {
                while (displayed->length > 0) {
                        g_queue_insert_sorted(
                            waiting, g_queue_pop_head(displayed), notification_cmp_data, NULL);
                }
                return;
        }

        /* move notifications back to queue, which are set to pushback */
        if (fullscreen) {
                GList *iter = g_queue_peek_head_link(displayed);
                while (iter) {
                        notification *n = iter->data;
                        GList *nextiter = iter->next;

                        if (n->fullscreen == FS_PUSHBACK){
                                g_queue_delete_link(displayed, iter);
                                g_queue_insert_sorted(waiting, n, notification_cmp_data, NULL);
                        }

                        iter = nextiter;
                }
        }

        /* move notifications from queue to displayed */
        GList *iter = g_queue_peek_head_link(waiting);
        while (iter) {
                notification *n = iter->data;
                GList *nextiter = iter->next;

                if (displayed_limit > 0 && displayed->length >= displayed_limit) {
                        /* the list is full */
                        break;
                }

                if (!n)
                        return;

                if (fullscreen
                    && (n->fullscreen == FS_DELAY || n->fullscreen == FS_PUSHBACK)) {
                        iter = nextiter;
                        continue;
                }

                n->start = time_monotonic_now();

                if (!n->redisplayed && n->script) {
                        notification_run_script(n);
                }

                g_queue_delete_link(waiting, iter);
                g_queue_insert_sorted(displayed, n, notification_cmp_data, NULL);

                iter = nextiter;
        }
}

/* see queues.h */
gint64 queues_get_next_datachange(gint64 time)
{
        gint64 sleep = G_MAXINT64;

        for (GList *iter = g_queue_peek_head_link(displayed); iter;
                        iter = iter->next) {
                notification *n = iter->data;
                gint64 ttl = n->timeout - (time - n->start);

                if (n->timeout > 0) {
                        if (ttl > 0)
                                sleep = MIN(sleep, ttl);
                        else
                                // while we're processing, the notification already timed out
                                return 0;
                }

                if (settings.show_age_threshold >= 0) {
                        gint64 age = time - n->timestamp;

                        if (age > settings.show_age_threshold)
                                // sleep exactly until the next shift of the second happens
                                sleep = MIN(sleep, ((G_USEC_PER_SEC) - (age % (G_USEC_PER_SEC))));
                        else if (n->timeout == 0 || ttl > settings.show_age_threshold)
                                sleep = MIN(sleep, settings.show_age_threshold);
                }
        }

        return sleep != G_MAXINT64 ? sleep : -1;
}

/* see queues.h */
void queues_pause_on(void)
{
        pause_displayed = true;
}

/* see queues.h */
void queues_pause_off(void)
{
        pause_displayed = false;
}

/* see queues.h */
bool queues_pause_status(void)
{
        return pause_displayed;
}

/**
 * Helper function for teardown_queues() to free a single notification
 *
 * @param data The notification to free
 */
static void teardown_notification(gpointer data)
{
        notification *n = data;
        notification_free(n);
}

/* see queues.h */
void teardown_queues(void)
{
        g_queue_free_full(history, teardown_notification);
        g_queue_free_full(displayed, teardown_notification);
        g_queue_free_full(waiting, teardown_notification);
}
/* vim: set tabstop=8 shiftwidth=8 expandtab textwidth=0: */
