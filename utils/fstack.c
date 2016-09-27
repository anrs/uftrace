#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>

/* This should be defined before #include "utils.h" */
#define PR_FMT     "fstack"
#define PR_DOMAIN  DBG_FSTACK

#include "uftrace.h"
#include "utils/utils.h"
#include "utils/filter.h"
#include "utils/fstack.h"
#include "utils/rbtree.h"
#include "libmcount/mcount.h"


#define FILTER_COUNT_NOTRACE  10000

bool fstack_enabled = true;

static enum filter_mode fstack_filter_mode = FILTER_MODE_NONE;

struct ftrace_task_handle *get_task_handle(struct ftrace_file_handle *handle,
					   int tid)
{
	int i;

	for (i = 0; i < handle->nr_tasks; i++) {
		if (handle->tasks[i].tid == tid)
			return &handle->tasks[i];
	}
	return NULL;
}

void setup_task_handle(struct ftrace_file_handle *handle,
		       struct ftrace_task_handle *task, int tid)
{
	int i;
	char *filename;
	int max_stack;

	xasprintf(&filename, "%s/%d.dat", handle->dirname, tid);

	memset(task, 0, sizeof(*task));

	task->h = handle;
	task->t = find_task(tid);

	task->tid = tid;
	task->fp = fopen(filename, "rb");
	if (task->fp == NULL) {
		pr_dbg("cannot open task data file: %s: %m\n", filename);
		task->done = true;
	}
	else
		pr_dbg2("opening %s\n", filename);

	free(filename);

	task->list_count = 0;
	task->stack_count = 0;
	task->column_index = -1;
	task->filter.depth = handle->depth;

	max_stack = handle->hdr.max_stack;
	task->func_stack = xcalloc(1, sizeof(*task->func_stack) * max_stack);

	INIT_LIST_HEAD(&task->rstack_list);
	INIT_LIST_HEAD(&task->rstack_unused);

	/* FIXME: save filter depth at fork() and restore */
	for (i = 0; i < max_stack; i++)
		task->func_stack[i].orig_depth = handle->depth;
}

void reset_task_handle(struct ftrace_file_handle *handle)
{
	int i;
	struct ftrace_task_handle *task;

	for (i = 0; i < handle->nr_tasks; i++) {
		task = &handle->tasks[i];

		task->done = true;

		if (task->fp) {
			fclose(task->fp);
			task->fp = NULL;
		}

		free(task->args.data);
		task->args.data = NULL;

		free(task->func_stack);
		task->func_stack = NULL;
	}

	free(handle->tasks);
	handle->tasks = NULL;

	handle->nr_tasks = 0;
}

/**
 * setup_task_filter - setup task filters using tid
 * @tid_filter - CSV of tid (or possibly separated by  ':')
 * @handle     - file handle
 *
 * This function sets up task filters using @tid_filter.
 * Tasks not listed will be ignored.
 */
void setup_task_filter(char *tid_filter, struct ftrace_file_handle *handle)
{
	int i, k;
	int nr_filters = 0;
	int *filter_tids = NULL;
	char *p = tid_filter;

	assert(tid_filter);

	do {
		int id;

		if (*p == ',' || *p == ':')
			p++;

		id = strtol(p, &p, 10);

		filter_tids = xrealloc(filter_tids, (nr_filters+1) * sizeof(int));
		filter_tids[nr_filters++] = id;

	} while (*p);

	handle->nr_tasks = handle->info.nr_tid;
	handle->tasks = xmalloc(sizeof(*handle->tasks) * handle->nr_tasks);

	for (i = 0; i < handle->nr_tasks; i++) {
		bool found = false;
		int tid = handle->info.tids[i];

		handle->tasks[i].tid = tid;

		for (k = 0; k < nr_filters; k++) {
			if (tid == filter_tids[k]) {
				found = true;
				break;
			}
		}

		if (!found) {
			memset(&handle->tasks[i], 0, sizeof(handle->tasks[i]));
			handle->tasks[i].done = true;
			continue;
		}

		setup_task_handle(handle, &handle->tasks[i], tid);
	}

	free(filter_tids);
}

static int setup_filters(struct ftrace_session *s, void *arg)
{
	char *filter_str = arg;
	LIST_HEAD(modules);

	ftrace_setup_filter_module(filter_str, &modules);
	load_module_symtabs(&s->symtabs, &modules);

	ftrace_setup_filter(filter_str, &s->symtabs, NULL, &s->filters,
			    &fstack_filter_mode);
	ftrace_setup_filter(filter_str, &s->symtabs, "PLT", &s->filters,
			    &fstack_filter_mode);
	ftrace_setup_filter(filter_str, &s->symtabs, "kernel", &s->filters,
			    &fstack_filter_mode);

	ftrace_cleanup_filter_module(&modules);
	return 0;
}

static int setup_trigger(struct ftrace_session *s, void *arg)
{
	char *trigger_str = arg;
	LIST_HEAD(modules);

	ftrace_setup_filter_module(trigger_str, &modules);
	load_module_symtabs(&s->symtabs, &modules);

	ftrace_setup_trigger(trigger_str, &s->symtabs, NULL, &s->filters);
	ftrace_setup_trigger(trigger_str, &s->symtabs, "PLT", &s->filters);
	ftrace_setup_trigger(trigger_str, &s->symtabs, "kernel", &s->filters);

	ftrace_cleanup_filter_module(&modules);
	return 0;
}

static int count_filters(struct ftrace_session *s, void *arg)
{
	int *count = arg;
	struct rb_node *node = rb_first(&s->filters);

	while (node) {
		(*count)++;
		node = rb_next(node);
	}
	return 0;
}

/**
 * setup_fstack_filters - setup symbol filters and triggers
 * @filter_str  - CSV of filter symbol names
 * @trigger_str - CSV of trigger definitions
 *
 * This function sets up the symbol filters and triggers using following syntax:
 *   filter_strs = filter | filter ";" filter_strs
 *   filter      = symbol | symbol "@" trigger
 *   trigger     = trigger_def | trigger_def "," trigger
 *   trigger_def = "depth=" NUM | "backtrace"
 */
int setup_fstack_filters(char *filter_str, char *trigger_str)
{
	int count = 0;

	if (filter_str) {
		walk_sessions(setup_filters, filter_str);
		walk_sessions(count_filters, &count);

		if (count == 0)
			return -1;
	}

	if (trigger_str) {
		int prev = count;

		walk_sessions(setup_trigger, trigger_str);
		walk_sessions(count_filters, &count);

		if (prev == count)
			return -1;
	}

	return 0;
}

static const char *fixup_syms[] = {
	"execl", "execlp", "execle", "execv", "execvp", "execvpe",
	"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp",
	"longjmp", "siglongjmp", "__longjmp_chk",
};

static int setjmp_depth;
static int setjmp_count;

static int build_fixup_filter(struct ftrace_session *s, void *arg)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fixup_syms); i++) {
		ftrace_setup_trigger((char *)fixup_syms[i], &s->symtabs, NULL,
				     &s->fixups);
	}
	return 0;
}

/**
 * fstack_prepare_fixup - setup special filters for fixup routines
 *
 * This function sets up special symbol filter tables which need
 * special handling like fork/exec, setjmp/longjmp cases.
 */
void fstack_prepare_fixup(void)
{
	walk_sessions(build_fixup_filter, NULL);
}

static int build_arg_spec(struct ftrace_session *s, void *arg)
{
	char *argspec = arg;
	LIST_HEAD(modules);

	ftrace_setup_filter_module(argspec, &modules);
	load_module_symtabs(&s->symtabs, &modules);

	ftrace_setup_argument(argspec, &s->symtabs, NULL, &s->filters);
	ftrace_setup_argument(argspec, &s->symtabs, "PLT", &s->filters);
	ftrace_setup_argument(argspec, &s->symtabs, "kernel", &s->filters);

	ftrace_cleanup_filter_module(&modules);
	return 0;
}

void setup_fstack_args(char *argspec)
{
	walk_sessions(build_arg_spec, argspec);
}

/**
 * fstack_entry - function entry handler
 * @task    - tracee task
 * @rstack  - function return stack
 * @tr      - trigger data
 *
 * This function should be called when replaying a recorded session.
 * It updates function stack, filter status, trigger result and
 * determine how to react. Callers can do whatever they want based
 * on the trigger result.
 *
 * This function returns -1 if it should be skipped, 0 otherwise.
 */
int fstack_entry(struct ftrace_task_handle *task,
		 struct ftrace_ret_stack *rstack,
		 struct ftrace_trigger *tr)
{
	struct fstack *fstack;
	struct ftrace_session *sess;
	unsigned long addr = rstack->addr;

	/* stack_count was increased in __read_rstack */
	fstack = &task->func_stack[task->stack_count - 1];

	pr_dbg2("ENTRY: [%5d] stack: %d, depth: %d, I: %d, O: %d, D: %d, flags = %lx %s\n",
		task->tid, task->stack_count-1, rstack->depth, task->filter.in_count,
		task->filter.out_count, task->filter.depth, fstack->flags,
		rstack->more ? "more" : "");

	fstack->orig_depth = task->filter.depth;
	fstack->flags = 0;

	if (task->filter.out_count > 0) {
		fstack->flags |= FSTACK_FL_NORECORD;
		return -1;
	}

	if (is_kernel_address(addr))
		addr = get_real_address(addr);

	sess = find_task_session(task->tid, rstack->time);
	if (sess == NULL)
		sess = find_task_session(task->t->pid, rstack->time);

	if (sess) {
		struct ftrace_filter *fixup;

		fixup = ftrace_match_filter(&sess->fixups, addr, tr);
		if (unlikely(fixup)) {
			if (!strncmp(fixup->name, "exec", 4))
				fstack->flags |= FSTACK_FL_EXEC;
			else if (strstr(fixup->name, "setjmp")) {
				setjmp_depth = task->display_depth + 1;
				setjmp_count = task->stack_count;
			}
			else if (strstr(fixup->name, "longjmp"))
				fstack->flags |= FSTACK_FL_LONGJMP;
		}

		ftrace_match_filter(&sess->filters, addr, tr);
	}


	if (tr->flags & TRIGGER_FL_FILTER) {
		if (tr->fmode == FILTER_MODE_IN) {
			task->filter.in_count++;
			fstack->flags |= FSTACK_FL_FILTERED;
		}
		else {
			task->filter.out_count++;
			fstack->flags |= FSTACK_FL_NOTRACE | FSTACK_FL_NORECORD;
			return -1;
		}

		/* restore default filter depth */
		task->filter.depth = task->h->depth;
	}
	else {
		if (fstack_filter_mode == FILTER_MODE_IN &&
		    task->filter.in_count == 0) {
			fstack->flags |= FSTACK_FL_NORECORD;
			return -1;
		}
	}

	if (tr->flags & TRIGGER_FL_DEPTH)
		task->filter.depth = tr->depth;

	if (tr->flags & TRIGGER_FL_TRACE_ON)
		fstack_enabled = true;

	if (tr->flags & TRIGGER_FL_TRACE_OFF)
		fstack_enabled = false;

	if (!fstack_enabled) {
		/*
		 * don't set NORECORD flag so that it can be printed
		 * when trace-on again
		 */
		return -1;
	}

	if (task->filter.depth <= 0) {
		fstack->flags |= FSTACK_FL_NORECORD;
		return -1;
	}

	task->filter.depth--;

	return 0;
}

/**
 * fstack_exit - function exit handler
 * @task - tracee task
 *
 * This function should be paired with fstack_entry().
 */
void fstack_exit(struct ftrace_task_handle *task)
{
	struct fstack *fstack;

	fstack = &task->func_stack[task->stack_count];

	pr_dbg2("EXIT : [%5d] stack: %d, depth: %d, I: %d, O: %d, D: %d, flags = %lx\n",
		task->tid, task->stack_count, fstack->orig_depth, task->filter.in_count,
		task->filter.out_count, task->filter.depth, fstack->flags);

	if (fstack->flags & FSTACK_FL_FILTERED)
		task->filter.in_count--;
	else if (fstack->flags & FSTACK_FL_NOTRACE)
		task->filter.out_count--;

	fstack->flags = 0;
	task->filter.depth = fstack->orig_depth;
}

/**
 * fstack_update - Update fstack related info
 * @type   - FTRACE_ENTRY or FTRACE_EXIT
 * @task   - tracee task
 * @fstack - function tracing stack
 *
 * This funciton updates current display depth according to @type and
 * flags of @fstack, and return a new depth.
 */
int fstack_update(int type, struct ftrace_task_handle *task,
		  struct fstack *fstack)
{
	if (type == FTRACE_ENTRY) {
		if (fstack->flags & FSTACK_FL_EXEC) {
			task->display_depth = 0;
			task->stack_count = 0;
			/* these are user functions */
			task->user_display_depth = 0;
			task->user_stack_count = 0;
		}
		else if (fstack->flags & FSTACK_FL_LONGJMP) {
			task->display_depth = setjmp_depth;
			task->stack_count = setjmp_count;
			/* these are user functions */
			task->user_display_depth = setjmp_depth;
			task->user_stack_count = setjmp_count;
		}
		else {
			task->display_depth++;
			if (task->ctx == FSTACK_CTX_USER) {
				task->user_display_depth++;
			}
		}

		fstack->flags &= ~(FSTACK_FL_EXEC | FSTACK_FL_LONGJMP);
	}
	else if (type == FTRACE_EXIT) {
		if (task->display_depth > 0)
			task->display_depth--;
		else
			task->display_depth = 0;

		if (task->ctx == FSTACK_CTX_USER) {
			if (task->user_display_depth > 0)
				task->user_display_depth--;
			else
				task->user_display_depth = 0;
		}
	}
	else {
		pr_err_ns("wrong type of fstack entry: %d\n", type);
	}
	return task->display_depth;
}

/* returns -1 if it can skip the rstack */
static int fstack_check_skip(struct ftrace_task_handle *task,
			     struct ftrace_ret_stack *rstack)
{
	struct ftrace_session *sess;
	unsigned long addr = rstack->addr;
	struct ftrace_trigger tr = { 0 };
	int depth = task->filter.depth;

	if (task->filter.out_count > 0)
		return -1;

	sess = find_task_session(task->tid, rstack->time);
	if (sess == NULL)
		sess = find_task_session(task->t->pid, rstack->time);

	if (sess == NULL) {
		if (is_kernel_address(addr))
			sess = first_session;
		else
			return -1;
	}

	ftrace_match_filter(&sess->filters, addr, &tr);

	if (tr.flags & TRIGGER_FL_FILTER) {
		if (tr.fmode == FILTER_MODE_OUT)
			return -1;

		depth = task->h->depth;
	}
	else if (fstack_filter_mode == FILTER_MODE_IN &&
		 task->filter.in_count == 0) {
			return -1;
	}

	if (tr.flags & (TRIGGER_FL_DEPTH | TRIGGER_FL_TRACE_ON))
		return 1;

	if (tr.flags & TRIGGER_FL_TRACE_OFF || depth <= 0)
		return -1;

	return 0;
}

/**
 * fstack_skip - Skip filtered record as many as possible
 * @handle     - file handle
 * @task       - tracee task
 * @curr_depth - current rstack depth
 *
 * This function checks next rstack and skip if it's filtered out.
 * The intention is to merge EXIT record after skipped ones.  It
 * returns updated @task pointer which contains next non-filtered
 * rstack or NULL if it's the last record.
 */
struct ftrace_task_handle *fstack_skip(struct ftrace_file_handle *handle,
				       struct ftrace_task_handle *task,
				       int curr_depth)
{
	struct ftrace_task_handle *next = NULL;
	struct fstack *fstack;
	struct ftrace_ret_stack *curr_stack = task->rstack;

	fstack = &task->func_stack[task->stack_count - 1];
	if (fstack->flags & (FSTACK_FL_EXEC | FSTACK_FL_LONGJMP))
		return NULL;

	if (peek_rstack(handle, &next) < 0)
		return NULL;

	/*
	 * different rstack means a context change between user and kernel,
	 * so the depth was increased and it needs checking.
	 */
	while (next == task && (curr_stack != next->rstack ||
				next->rstack->depth > curr_depth)) {
		struct ftrace_ret_stack *next_stack = next->rstack;
		struct ftrace_trigger tr = { 0 };

		/* return if it's not filtered */
		if (next_stack->type == FTRACE_ENTRY) {
			if (fstack_check_skip(task, next_stack) >= 0)
				break;
		}
		else if (next_stack->type != FTRACE_EXIT)
			return NULL;

		/* consume the filtered rstack */
		if (read_rstack(handle, &next) < 0)
			pr_err("error during skip rstack");

		/*
		 * call fstack_entry/exit() after read_rstack() so
		 * that it can changes stack_count properly.
		 */
		if (next_stack->type == FTRACE_ENTRY)
			fstack_entry(task, next_stack, &tr);
		else
			fstack_exit(task);

		if (!fstack_enabled)
			return NULL;

		/* and then read next */
		if (peek_rstack(handle, &next) < 0)
			return NULL;
	}

	return next;
}

static int __read_task_ustack(struct ftrace_task_handle *task)
{
	FILE *fp = task->fp;

	if (fread(&task->ustack, sizeof(task->ustack), 1, fp) != 1) {
		if (feof(fp))
			return -1;

		pr_log("error reading rstack: %s\n", strerror(errno));
		return -1;
	}

	if (task->ustack.unused != FTRACE_UNUSED) {
		pr_dbg("invalid rstack read\n");
		return -1;
	}

	return 0;
}

static int read_task_arg(struct ftrace_task_handle *task,
			 struct ftrace_arg_spec *spec)
{
	FILE *fp = task->fp;
	struct fstack_arguments *args = &task->args;
	unsigned size = spec->size;
	int rem;

	if (spec->fmt == ARG_FMT_STR) {
		args->data = xrealloc(args->data, args->len + 2);

		if (fread(args->data + args->len, 2, 1, fp) != 1) {
			if (feof(fp))
				return -1;
		}

		size = *(unsigned short *)(args->data + args->len);
		args->len += 2;
	}

	args->data = xrealloc(args->data, args->len + size);

	if (fread(args->data + args->len, size, 1, fp) != 1) {
		if (feof(fp))
			return -1;
	}

	args->len += size;

	rem = args->len % 4;
	if (rem) {
		fseek(fp, 4 - rem, SEEK_CUR);
		args->len += 4 - rem;
	}

	return 0;
}

/**
 * read_task_args - read arguments of current function of the task
 * @task: tracee task
 * @rstack: ftrace_ret_stack
 * @is_retval: 0 reads argument, 1 reads return value
 *
 * This function reads argument records of @task's current function
 * according to the @spec.
 */
int read_task_args(struct ftrace_task_handle *task,
		   struct ftrace_ret_stack *rstack,
		   bool is_retval)
{
	struct ftrace_session *sess;
	struct ftrace_trigger tr = {};
	struct ftrace_filter *fl;
	struct ftrace_arg_spec *arg;
	int rem;

	sess = find_task_session(task->tid, rstack->time);
	if (sess == NULL) {
		pr_dbg("cannot find session\n");
		return -1;
	}

	fl = ftrace_match_filter(&sess->filters, rstack->addr, &tr);
	if (fl == NULL) {
		pr_dbg("cannot find filter: %lx\n", rstack->addr);
		return -1;
	}
	if (!(tr.flags & (TRIGGER_FL_ARGUMENT | TRIGGER_FL_RETVAL))) {
		pr_dbg("cannot find arg spec\n");
		return -1;
	}

	task->args.len = 0;
	task->args.args = &fl->args;

	list_for_each_entry(arg, &fl->args, list) {
		/* skip unwanted arguments or retval */
		if (is_retval != (arg->idx == RETVAL_IDX))
			continue;

		if (read_task_arg(task, arg) < 0)
			return -1;
	}

	rem = task->args.len % 8;
	if (rem)
		fseek(task->fp, 8 - rem, SEEK_CUR);

	return 0;
}

/**
 * read_task_ustack - read user function record for @task
 * @handle: file handle
 * @task: tracee task
 *
 * This function reads current ftrace rcord and save it to @task->ustack.
 * Data file it accesses should be opened already.  When @task->valid is
 * set, it just returns @task->ustack already read, so if you want to force
 * read from file, the @task->valid should be reset before calling this
 * function.
 *
 * This function returns 0 if succeeded, -1 otherwise.
 */
int read_task_ustack(struct ftrace_file_handle *handle,
		     struct ftrace_task_handle *task)
{
	if (task->valid)
		return 0;

	if (task->done || task->fp == NULL)
		return -1;

	if (__read_task_ustack(task) < 0) {
		task->done = true;
		fclose(task->fp);
		task->fp = NULL;
		return -1;
	}

	if (task->lost_seen) {
		int i;

		for (i = 0; i <= task->ustack.depth; i++)
			task->func_stack[i].valid = false;

		pr_dbg("lost seen: invalidating existing stack..\n");
		task->lost_seen = false;

		/* reset display depth after lost */
		task->display_depth_set = false;
	}

	if (task->ustack.more) {
		if (!(handle->hdr.feat_mask & (ARGUMENT | RETVAL)) ||
		    handle->info.argspec == NULL)
			pr_err_ns("invalid data (more bit set w/o args)");

		if (task->ustack.type == FTRACE_ENTRY)
			read_task_args(task, &task->ustack, false);
		else if (task->ustack.type == FTRACE_EXIT)
			read_task_args(task, &task->ustack, true);
		else
			abort();
	}

	task->valid = true;
	return 0;
}

/* returns rstack after time filter applied */
static struct ftrace_ret_stack * get_first_rstack(struct ftrace_task_handle *task)
{
	struct ftrace_ret_stack_list *entry;

	assert(task->list_count > 0);

	entry = list_first_entry(&task->rstack_list, typeof(*entry), list);
	memcpy(&task->ustack, &entry->rstack, sizeof(entry->rstack));

	return &task->ustack;
}

void invalidate_first_rstack(struct ftrace_task_handle *task)
{
	struct ftrace_ret_stack_list *entry;

	if (task->h->time_filter == 0)
		return;

	assert(task->list_count > 0);

	entry = list_first_entry(&task->rstack_list, typeof(*entry), list);
	list_move_tail(&entry->list, &task->rstack_unused);
	task->list_count--;
}

static void add_to_rstack_list(struct ftrace_task_handle *task)
{
	struct ftrace_ret_stack_list *entry;
	struct ftrace_ret_stack *rstack = &task->ustack;

	if (!list_empty(&task->rstack_unused)) {
		entry = list_first_entry(&task->rstack_unused,
					 typeof(*entry), list);
		list_del(&entry->list);
	}
	else {
		entry = xmalloc(sizeof(*entry));
	}

	memcpy(&entry->rstack, rstack, sizeof(*rstack));

	list_add_tail(&entry->list, &task->rstack_list);
	task->list_count++;
}

static void delete_last_rstack_list(struct ftrace_task_handle *task)
{
	struct ftrace_ret_stack_list *entry;

	assert(task->list_count > 0);

	entry = list_last_entry(&task->rstack_list, typeof(*entry), list);
	list_move(&entry->list, &task->rstack_unused);
	task->list_count--;
}

/**
 * get_task_ustack - read task's user function record
 * @handle: file handle
 * @idx: task index
 *
 * This function returns current ftrace record of @idx-th task from
 * data file in @handle.
 */
struct ftrace_ret_stack *
get_task_ustack(struct ftrace_file_handle *handle, int idx)
{
	struct ftrace_task_handle *task;

	if (unlikely(handle->nr_tasks < handle->info.nr_tid)) {
		int i;

		handle->nr_tasks = handle->info.nr_tid;
		handle->tasks = xrealloc(handle->tasks,
					 sizeof(*handle->tasks) * handle->nr_tasks);

		for (i = 0; i < handle->info.nr_tid; i++) {
			setup_task_handle(handle, &handle->tasks[i],
					  handle->info.tids[i]);
		}

		if (handle->tasks[idx].fp == NULL)
			return NULL;
	}

	task = &handle->tasks[idx];

	if (!handle->time_filter) {
		if (read_task_ustack(handle, task) < 0)
			return NULL;
		return &task->ustack;
	}

	if (task->list_count)
		goto out;

	/*
	 * read task (user) stack until it founds an entry that exceeds
	 * the given time threshold (-t option).
	 */
	while (read_task_ustack(handle, task) == 0) {
		struct ftrace_ret_stack_list *last;
		struct ftrace_ret_stack *curr = &task->ustack;

		task->valid = false;

		if (task->ustack.type == FTRACE_ENTRY) {
			/* it needs to wait until matching exit found */
			add_to_rstack_list(task);
		}
		else if (task->ustack.type == FTRACE_EXIT) {
			uint64_t delta;

			/* it's already passed time filter, just return */
			if (task->list_count == 0) {
				add_to_rstack_list(task);
				break;
			}

			last = list_last_entry(&task->rstack_list,
					       typeof(*last), list);
			delta = curr->time - last->rstack.time;

			if (delta < handle->time_filter)
				delete_last_rstack_list(task);
			else {
				add_to_rstack_list(task);
				break;
			}
		}
		else {
			add_to_rstack_list(task);
			/* TODO: handle LOST properly */
			break;
		}
	}
	if (task->done && task->list_count == 0)
		return NULL;

out:
	task->valid = true;
	return get_first_rstack(task);
}

static int read_user_stack(struct ftrace_file_handle *handle,
			   struct ftrace_task_handle **task)
{
	int i, next_i = -1;
	uint64_t next_time;
	struct ftrace_ret_stack *tmp;

	for (i = 0; i < handle->info.nr_tid; i++) {
		tmp = get_task_ustack(handle, i);
		if (tmp == NULL)
			continue;

		if (next_i < 0 || tmp->time < next_time) {
			next_time = tmp->time;
			next_i = i;
		}
	}

	if (next_i < 0)
		return -1;

	*task = &handle->tasks[next_i];

	return next_i;
}

static int __read_rstack(struct ftrace_file_handle *handle,
			 struct ftrace_task_handle **taskp,
			 bool invalidate)
{
	int u, k = -1;
	struct ftrace_task_handle *task = NULL;
	struct ftrace_kernel *kernel = handle->kern;
	struct mcount_ret_stack kstack;
	struct fstack *fstack;
	uint64_t ktime;

	u = read_user_stack(handle, taskp);
	if (kernel) {
		k = read_kernel_stack(kernel, &kstack);
		if (k < 0) {
			static bool warn = false;

			if (!warn && invalidate) {
				pr_dbg("no more kernel data\n");
				warn = true;
			}
		}
	}

	if (u < 0 && k < 0)
		return -1;

	if (k < 0)
		goto user;
	if (u < 0)
		goto kernel;

	ktime = kstack.end_time ?: kstack.start_time;

	if ((*taskp)->ustack.time < ktime) {
user:
		task = *taskp;

		if (invalidate) {
			invalidate_first_rstack(task);
			task->valid = false;
		}

		task->rstack = &task->ustack;

		if (!task->display_depth_set) {
			/* inherit display_depth after [v]fork() */
			task->display_depth = task->ustack.depth;
			if (task->ustack.type == FTRACE_EXIT)
				task->display_depth++;
			task->display_depth_set = true;

			task->stack_count = task->display_depth;
			task->filter.depth = handle->depth - task->stack_count;
		}

		if (task->ctx == FSTACK_CTX_KERNEL && invalidate) {
			/* protect from broken kernel records */
			task->display_depth = task->user_display_depth;
			task->stack_count = task->user_stack_count;
			task->filter.depth = handle->depth - task->stack_count;
		}

		if (task->ustack.type == FTRACE_ENTRY) {
			fstack = &task->func_stack[task->stack_count];

			fstack->total_time = task->ustack.time;
			fstack->child_time = 0;
			fstack->valid = true;
			fstack->addr = task->ustack.addr;
		}
		else if (task->ustack.type == FTRACE_EXIT) {
			uint64_t delta;
			int idx = task->stack_count - 1;

			if (idx < 0)
				idx = 0;

			fstack = &task->func_stack[idx];

			delta = task->ustack.time - fstack->total_time;

			if (!fstack->valid)
				delta = 0UL;
			fstack->valid = false;

			fstack->total_time = delta;
			if (fstack->child_time > fstack->total_time)
				fstack->child_time = fstack->total_time;

			if (task->stack_count > 1)
				fstack[-1].child_time += delta;
		}
		else if (task->ustack.type == FTRACE_LOST) {
			task->lost_seen = true;
		}

		if (invalidate)
			task->ctx = FSTACK_CTX_USER;
	}
	else {
kernel:
		task = get_task_handle(handle, kstack.tid);
		if (task == NULL)
			pr_err_ns("cannot find task for tid %d\n", kstack.tid);

		if (kernel->missed_events[k]) {
			/* convert to ftrace_rstack */
			task->kstack.time = 0;
			task->kstack.type = FTRACE_LOST;
			task->kstack.addr = kernel->missed_events[k];
			task->kstack.depth = kstack.depth;
			task->kstack.unused = FTRACE_UNUSED;
			task->kstack.more = 0;

			/*
			 * NOTE: do not invalidate the kstack since we didn't
			 * read the first record yet.  Next read_kernel_stack()
			 * will return the first record.
			 */
		}
		else {
			/* convert to ftrace_rstack */
			task->kstack.time = kstack.end_time ?: kstack.start_time;
			task->kstack.type = kstack.end_time ? FTRACE_EXIT : FTRACE_ENTRY;
			task->kstack.addr = kstack.child_ip;
			task->kstack.depth = kstack.depth;
			task->kstack.unused = FTRACE_UNUSED;
			task->kstack.more = 0;

			if (invalidate) {
				kernel->rstack_valid[k] = false;
				task->lost_seen = false;
			}
		}

		task->rstack = &task->kstack;

		if (!task->display_depth_set) {
			/* kernel functions might start with >0 depth */
			task->display_depth = task->user_display_depth + task->kstack.depth;
			if (task->kstack.type == FTRACE_EXIT)
				task->display_depth++;
			task->display_depth_set = true;

			task->stack_count = task->user_stack_count + task->kstack.depth;
			task->filter.depth = handle->depth - task->stack_count;
		}

		if (task->rstack->type == FTRACE_ENTRY) {
			fstack = &task->func_stack[task->stack_count];

			fstack->valid = true;
			fstack->addr = kstack.child_ip;
			fstack->child_time = 0;
		}
		else if (task->rstack->type == FTRACE_EXIT) {
			int idx = task->stack_count - 1;
			uint64_t delta = kstack.end_time - kstack.start_time;

			if (idx < 0)
				idx = 0;

			fstack = &task->func_stack[idx];

			if (!fstack->valid) {
				delta = 0UL;
				fstack->addr = kstack.child_ip;
			}
			fstack->valid = false;

			fstack->total_time = delta;
			if (fstack->child_time > fstack->total_time)
				fstack->child_time = fstack->total_time;

			if (task->stack_count > 1) {
				uint64_t child_time = fstack->total_time;

				fstack[-1].child_time += child_time;
			}
		}
		else if (task->rstack->type == FTRACE_LOST) {
			int i;

			task->lost_seen = true;
			task->display_depth_set = false;

			for (i = task->user_stack_count; i <= task->stack_count; i++) {
				fstack = &task->func_stack[i];
				fstack->total_time = 0;
				fstack->valid = false;
			}
		}

		if (invalidate)
			task->ctx = FSTACK_CTX_KERNEL;
	}

	/* update stack count when the rstack is actually used */
	if (invalidate) {
		if (task->rstack->type == FTRACE_ENTRY)
			task->stack_count++;
		else if (task->rstack->type == FTRACE_EXIT &&
			 task->stack_count > 0)
			task->stack_count--;
		else if (task->rstack->type == FTRACE_LOST &&
			 task->ctx == FSTACK_CTX_KERNEL)
			kernel->missed_events[k] = 0;

		if (task->ctx == FSTACK_CTX_USER) {
			if (task->rstack->type == FTRACE_ENTRY)
				task->user_stack_count++;
			else if (task->rstack->type == FTRACE_EXIT &&
				 task->user_stack_count > 0)
				task->user_stack_count--;
		}
	}

	*taskp = task;
	return 0;
}

/**
 * read_rstack - read and consume the oldest ftrace stack
 * @handle: file handle
 * @task: pointer to the oldest task
 *
 * This function reads all function trace records of each task,
 * compares the timestamp, and find the oldest one.  After this
 * function @task will point a task which has the oldest record, and
 * it can be accessed by @task->rstack.  The oldest record will be
 * consumed, that means it sets another (*@task)->rstack for next
 * call.
 *
 * This function returns 0 if it reads a rstack, -1 if it's done.
 */
int read_rstack(struct ftrace_file_handle *handle,
		struct ftrace_task_handle **task)
{
	return __read_rstack(handle, task, true);
}

/**
 * peek_rstack - read the oldest ftrace stack
 * @handle: file handle
 * @task: pointer to the oldest task
 *
 * This function reads all function trace records of each task,
 * compares the timestamp, and find the oldest one.  After this
 * function @task will point a task which has the oldest record, and
 * it can be accessed by @task->rstack.  The oldest record will *NOT*
 * be consumed, that means another call to this or @read_rstack will
 * return same (*@task)->rstack.
 *
 * This function returns 0 if it reads a rstack, -1 if it's done.
 */
int peek_rstack(struct ftrace_file_handle *handle,
		struct ftrace_task_handle **task)
{
	return __read_rstack(handle, task, false);
}


#ifdef UNIT_TEST

#include <sys/stat.h>

#define NUM_TASK    2
#define NUM_RECORD  4

static int test_tids[NUM_TASK] = { 1234, 5678 };
static struct ftrace_task test_tasks[NUM_TASK];
static struct ftrace_ret_stack test_record[NUM_TASK][NUM_RECORD] = {
	{
		{ 100, FTRACE_ENTRY, false, FTRACE_UNUSED, 0, 0x40000 },
		{ 200, FTRACE_ENTRY, false, FTRACE_UNUSED, 1, 0x41000 },
		{ 300, FTRACE_EXIT,  false, FTRACE_UNUSED, 1, 0x41000 },
		{ 400, FTRACE_EXIT,  false, FTRACE_UNUSED, 0, 0x40000 },
	},
	{
		{ 150, FTRACE_ENTRY, false, FTRACE_UNUSED, 0, 0x40000 },
		{ 250, FTRACE_ENTRY, false, FTRACE_UNUSED, 1, 0x41000 },
		{ 350, FTRACE_EXIT,  false, FTRACE_UNUSED, 1, 0x41000 },
		{ 450, FTRACE_EXIT,  false, FTRACE_UNUSED, 0, 0x40000 },
	}
};

static struct ftrace_file_handle fstack_test_handle;
static void fstack_test_finish_file(void);

static int fstack_test_setup_file(struct ftrace_file_handle *handle, int nr_tid)
{
	int i;
	char *filename;

	handle->dirname = "tmp.dir";
	handle->info.tids = test_tids;
	handle->info.nr_tid = nr_tid;
	handle->hdr.max_stack = 16;

	if (mkdir(handle->dirname, 0755) < 0) {
		if (errno != EEXIST) {
			pr_dbg("cannot create temp dir: %m\n");
			return -1;
		}
	}

	for (i = 0; i < handle->info.nr_tid; i++) {
		FILE *fp;

		if (asprintf(&filename, "%s/%d.dat",
			     handle->dirname, handle->info.tids[i]) < 0) {
			pr_dbg("cannot alloc filename: %s/%d.dat",
			       handle->dirname, handle->info.tids[i]);
			return -1;
		}

		fp = fopen(filename, "w");
		if (fp == NULL) {
			pr_dbg("file open failed: %m\n");
			free(filename);
			return -1;
		}

		fwrite(test_record[i], sizeof(test_record[i][0]),
		       ARRAY_SIZE(test_record[i]), fp);

		free(filename);
		fclose(fp);

		test_tasks[i].tid = handle->info.tids[i];
	}

	atexit(fstack_test_finish_file);
	return 0;
}

static void fstack_test_finish_file(void)
{
	int i;
	char *filename;
	struct ftrace_file_handle *handle = &fstack_test_handle;

	if (handle->dirname == NULL)
		return;

	reset_task_handle(handle);

	for (i = 0; i < handle->info.nr_tid; i++) {
		if (asprintf(&filename, "%s/%d.dat",
			     handle->dirname, handle->info.tids[i]) < 0)
			return;

		remove(filename);
		free(filename);
	}
	remove(handle->dirname);
	handle->dirname = NULL;
}

TEST_CASE(fstack_read)
{
	struct ftrace_file_handle *handle = &fstack_test_handle;
	struct ftrace_task_handle *task;
	int i;

	TEST_EQ(fstack_test_setup_file(handle, ARRAY_SIZE(test_tids)), 0);

	for (i = 0; i < NUM_RECORD; i++) {
		TEST_EQ(read_rstack(handle, &task), 0);
		TEST_EQ(task->tid, test_tids[0]);
		TEST_EQ((uint64_t)task->rstack->type,  (uint64_t)test_record[0][i].type);
		TEST_EQ((uint64_t)task->rstack->depth, (uint64_t)test_record[0][i].depth);
		TEST_EQ((uint64_t)task->rstack->addr,  (uint64_t)test_record[0][i].addr);

		TEST_EQ(peek_rstack(handle, &task), 0);
		TEST_EQ(task->tid, test_tids[1]);
		TEST_EQ((uint64_t)task->rstack->type,  (uint64_t)test_record[1][i].type);
		TEST_EQ((uint64_t)task->rstack->depth, (uint64_t)test_record[1][i].depth);
		TEST_EQ((uint64_t)task->rstack->addr,  (uint64_t)test_record[1][i].addr);

		TEST_EQ(read_rstack(handle, &task), 0);
		TEST_EQ(task->tid, test_tids[1]);
		TEST_EQ((uint64_t)task->rstack->type,  (uint64_t)test_record[1][i].type);
		TEST_EQ((uint64_t)task->rstack->depth, (uint64_t)test_record[1][i].depth);
		TEST_EQ((uint64_t)task->rstack->addr,  (uint64_t)test_record[1][i].addr);
	}

	return TEST_OK;
}

TEST_CASE(fstack_skip)
{
	struct ftrace_file_handle *handle = &fstack_test_handle;
	struct ftrace_task_handle *task;
	struct ftrace_trigger tr = { 0, };
	int i;

	dbg_domain[DBG_FSTACK] = 1;

	TEST_EQ(fstack_test_setup_file(handle, 1), 0);

	/* this makes to skip depth 1 records */
	handle->depth = 1;

	TEST_EQ(read_rstack(handle, &task), 0);

	/* for fstack_entry not to crash */
	task->t = &test_tasks[0];

	TEST_EQ(fstack_entry(task, task->rstack, &tr), 0);
	TEST_EQ(task->tid, test_tids[0]);
	TEST_EQ((uint64_t)task->rstack->type,  (uint64_t)test_record[0][0].type);
	TEST_EQ((uint64_t)task->rstack->depth, (uint64_t)test_record[0][0].depth);
	TEST_EQ((uint64_t)task->rstack->addr,  (uint64_t)test_record[0][0].addr);

	/* skip filtered records (due to depth) */
	TEST_EQ(fstack_skip(handle, task, task->rstack->depth), task);
	TEST_EQ(task->tid, test_tids[0]);
	TEST_EQ((uint64_t)task->rstack->type,  (uint64_t)test_record[0][3].type);
	TEST_EQ((uint64_t)task->rstack->depth, (uint64_t)test_record[0][3].depth);
	TEST_EQ((uint64_t)task->rstack->addr,  (uint64_t)test_record[0][3].addr);

	return TEST_OK;
}

#endif /* UNIT_TEST */
