# mx_object_get_info

## NAME

object_get_info - query information about an object

## SYNOPSIS

```
#include <magenta/syscalls.h>
#include <magenta/syscalls/object.h>

mx_status_t mx_object_get_info(mx_handle_t handle, uint32_t topic,
                               void* buffer, size_t buffer_size,
                               size_t* actual, size_t* avail);

```

## DESCRIPTION

**object_get_info()** requests information about the provided handle (or the object
the handle refers to).  The *topic* parameter indicates what specific information is desired.

*buffer* is a pointer to a buffer of size *buffer_size* to return the information.

*actual* is an optional pointer to return the number of records that were written to buffer.

*avail* is an optional pointer to return the number of records that are available to read.

If the buffer is insufficiently large, *avail* will be larger than *actual*.


## TOPICS

**MX_INFO_HANDLE_VALID**  No records are returned and *buffer* may be NULL.  This query
succeeds as long as *handle* is a valid handle.

**MX_INFO_HANDLE_BASIC**  Always returns a single *mx_info_handle_basic_t* record containing
information about the handle:

```
typedef struct mx_info_handle_basic {
    mx_koid_t koid;
    mx_rights_t rights;
    uint32_t type;
    mx_koid_t related_koid;
    uint32_t props;
} mx_info_handle_basic_t;

```
*koid* is the unique id assigned by kernel to the object referenced by the handle.

*rights* are the immutable rights assigned to the handle. Two handles that have the same koid
and the same rights are equivalent and interchangeable.

*type* is the object type. Allows to tell appart channels, events, sockets, etc.

*related_koid* is the koid of the logical counterpart or parent object of the object
referenced by the handle. Otherwise this value is zero.

*props* contains zero or **MX_OBJ_PROP_WAITABLE** if the object referenced by the
handle can be waited on.

**MX_INFO_PROCESS**  Requires a Process handle.  Always returns a single *mx_info_process_t*
record containing:

*   *return_code*: The process's return code; only valid if *exited* is true.
    Guaranteed to be non-zero if the process was killed by *mx_task_kill*.
*   *started*: True if the process has ever left the initial creation state, even if it has
    exited as well.
*   *exited*: If true, the process has exited and *return_code* is valid.
*   *debugger_attached*: True if a debugger is attached to the process.

**MX_INFO_PROCESS_THREADS**  Requires a Process handle. Returns an array of *mx_koid_t*, one for
each thread in the Process at that moment in time.

**MX_INFO_RESOURCE_CHILDREN**  Requires a Resource handle.  Returns an array of *mx_rrec_t*,
one for each child Resource of the provided Resource handle.

**MX_INFO_RESOURCE_RECORDS**  Requires a Resource handle.  Returns an array of *mx_rrec_t*,
one for each Record associated with the provided Resource handle.

**MX_INFO_THREAD**  Requires a Thread handle.  Always returns a single *mx_info_thread_t*
record containing:

*   *exception_port_type*: Zero if the thread is not in an exception, or a
    non-zero value indicating the kind of exception port that the thread is
    waiting for an exception response from. The value is represented by these
    values defined in magenta/syscalls/exception.h:

    - *MX_EXCEPTION_PORT_TYPE_NONE*
    - *MX_EXCEPTION_PORT_TYPE_DEBUGGER*
    - *MX_EXCEPTION_PORT_TYPE_THREAD*
    - *MX_EXCEPTION_PORT_TYPE_PROCESS*
    - *MX_EXCEPTION_PORT_TYPE_SYSTEM*

**MX_INFO_THREAD_EXCEPTION_REPORT**  Requires a Thread handle. If the thread is
currently in an exception and is waiting for an exception response, then
this returns the exception report as a *mx_exception_report_t*.

**MX_INFO_VMAR**  Requires a VM Address Region handle.  Always returns a single *mx_info_vmar_t*
record containing the base and length of the region.

**MX_INFO_JOB_CHILDREN**  Requires a Job handle. Returns an array of
  *mx_koid_t*s corresponding to the direct child Jobs of the given Job.

**MX_INFO_JOB_PROCESSES**  Requires a Job handle. Returns an array of
  *mx_koid_t*s corresponding to the direct child Processes of the given Job.

**MX_INFO_TASK_STATS** Requires a Process handle. May be somewhat expensive
to gather. Always returns a single *mx_info_task_stats_t* record containing:

*   *mem_mapped_bytes*: The total size of mapped memory ranges in the task.
    Not all will be backed by physical memory.
*   *mem_committed_bytes*: The amount of mapped address space backed by
    physical memory. Will be no larger than mem_mapped_bytes. Some of the
    pages may be double-mapped (and thus double-counted), or may be shared with
    other tasks.


## RETURN VALUE

**mx_object_get_info**() returns **NO_ERROR** on success. In the event of failure, a negative error
value is returned.

## ERRORS

**ERR_BAD_HANDLE**  *handle* is not a valid handle.

**ERR_WRONG_TYPE**  *handle* is not an appropriate type for *topic*

**ERR_INVALID_ARGS**  *buffer*, *actual*, or *avail* are invalid pointers.

**ERR_NO_MEMORY**  Temporary out of memory failure.

**ERR_BUFFER_TOO_SMALL**  The *topic* returns a fixed number of records, but the provided buffer
is not large enough for these records.

**ERR_NOT_SUPPORTED**  *topic* does not exist.


## EXAMPLES

```
bool is_handle_valid(mx_handle_t handle) {
    return mx_object_get_info(handle, MX_INFO_HANDLE_VALID, NULL, 0, NULL, NULL) == NO_ERROR;
}


mx_koid_t get_object_koid(mx_handle_t handle) {
    mx_info_handle_basic_t info;
    if (mx_object_get_info(handle, MX_INFO_HANDLE_BASIC, &info, sizeof(info), NULL, NULL)) {
        return 0;
    } else {
        return info.koid;
    }
}


mx_koid_t threads[128];
size_t count, avail;

if (mx_object_get_info(proc, MX_INFO_PROCESS_THREADS, threads,
                       sizeof(threads), &count, &avail) < 0) {
    // error!
} else {
    if (avail > count) {
        // more threads than space in array, could call again with larger array
    }
    for (unsigned n = 0; n < count; n++) {
        do_something(thread[n]);
    }
}
```


## SEE ALSO

[handle_close](handle_close.md),
[handle_duplicate](handle_duplicate.md),
[handle_replace](handle_replace.md),
[object_get_child](object_get_child.md).
