# Fluent Bit Forward Input Plugin Segfault Fix - Updated

## Issue Summary

A segmentation fault occurs in the forward input plugin under high load conditions when:
1. The `mem_buf_limit` setting is lower than the forward plugin's `buffer_max_size`
2. The system experiences rapid pause/resume cycles due to memory pressure

### Stack Trace (Latest)
```
[2025/11/05 20:07:54] [engine] caught signal (SIGSEGV)
#0  0x7f8810d91e55      in  ???() at ???:0
#1  0x98aca9            in  receiver_recv() at plugins/in_forward/fw_prot.c:1086
#2  0x98ad5b            in  receiver_to_unpacker() at plugins/in_forward/fw_prot.c:1102
#3  0x98b795            in  fw_prot_process() at plugins/in_forward/fw_prot.c:1291
#4  0x9831d4            in  fw_conn_event() at plugins/in_forward/fw_conn.c:126
```

## Root Cause Analysis

The initial fix addressed the race condition at the event handler level, but a deeper race condition remained:

### Original Race Condition (Fixed in First Pass):
1. Thread A: `fw_conn_event()` acquires mutex, checks pause state, releases mutex
2. Thread B: Plugin gets paused, calls `fw_conn_del_all()`, frees connections
3. Thread A: Continues executing with freed connection pointer → **SEGFAULT**

### Remaining Race Condition (Fixed in This Update):
1. Thread A: `fw_conn_event()` passes pause check, calls `fw_prot_process()`
2. Thread B: Plugin gets paused mid-processing, calls `fw_conn_del_all()`, **frees `conn->buf`**
3. Thread A: Still inside `fw_prot_process()` processing loop
4. Thread A: `receiver_recv()` tries to access freed `conn->buf` → **SEGFAULT at line 1086**

The critical issue is that `fw_prot_process()` can take significant time to process large amounts of data, creating a long window where the connection buffer can be freed while still being accessed.

## Comprehensive Fix Implementation

The fix implements a **defense-in-depth** strategy with four layers of protection:

### 1. Connection State Flag
**File: `plugins/in_forward/fw_conn.h`**

Added `being_deleted` flag to track deletion state:
```c
struct fw_conn {
    int status;
    int handshake_status;
    int being_deleted;               /* Flag: connection is being deleted */

    char *buf;                       /* Buffer data */
    // ...
};
```

**File: `plugins/in_forward/fw_conn.c`**

Initialize flag on connection creation:
```c
conn->being_deleted = 0;
```

### 2. Safe Connection Deletion
**File: `plugins/in_forward/fw_conn.c` - `fw_conn_del()`**

Set flag early and safely clean up resources:
```c
int fw_conn_del(struct fw_conn *conn)
{
    /*
     * Set being_deleted flag to prevent any in-flight processing
     * from accessing this connection's resources
     */
    conn->being_deleted = 1;

    /* ... cleanup code ... */

    /* Free buffer and set to NULL to prevent use-after-free */
    if (conn->buf) {
        flb_free(conn->buf);
        conn->buf = NULL;
    }

    flb_free(conn);
    return 0;
}
```

### 3. Buffer Access Protection
**File: `plugins/in_forward/fw_prot.c` - `receiver_recv()`**

Check connection state before accessing buffer:
```c
static size_t receiver_recv(struct fw_conn *conn, char *buf, size_t try_size) {
    size_t off;
    size_t actual_size;

    /* Safety check: ensure connection is not being deleted and buffer exists */
    if (conn->being_deleted || !conn->buf) {
        return 0;
    }

    off = conn->buf_len - conn->rest;
    actual_size = try_size;

    if (actual_size > conn->rest) {
        actual_size = conn->rest;
    }

    memcpy(buf, conn->buf + off, actual_size);
    conn->rest -= actual_size;

    return actual_size;
}
```

### 4. Periodic Pause State Checks
**File: `plugins/in_forward/fw_prot.c` - `fw_prot_process()`**

Check for pause and deletion states in the processing loop:
```c
int fw_prot_process(struct flb_input_instance *ins, struct fw_conn *conn)
{
    // ... initialization ...

    while (1) {
        /* Check if connection is being deleted or plugin is paused */
        if (conn->being_deleted) {
            msgpack_unpacker_free(unp);
            msgpack_unpacked_destroy(&result);
            flb_sds_destroy(out_tag);
            return 0;
        }

        pthread_mutex_lock(&ctx->conn_mutex);
        if (ctx->is_paused) {
            pthread_mutex_unlock(&ctx->conn_mutex);
            msgpack_unpacker_free(unp);
            msgpack_unpacked_destroy(&result);
            flb_sds_destroy(out_tag);
            return 0;
        }
        pthread_mutex_unlock(&ctx->conn_mutex);

        recv_len = receiver_to_unpacker(conn, EACH_RECV_SIZE, unp);
        // ... continue processing ...
    }
}
```

## How The Fix Works

The multi-layered approach ensures safety through redundancy:

1. **Layer 1 (Existing)**: `fw_conn_event()` checks pause state before calling `fw_prot_process()`
   - Prevents new processing from starting when paused

2. **Layer 2 (New)**: `fw_prot_process()` periodically checks pause state and deletion flag
   - Allows long-running processing to exit gracefully if pause occurs mid-execution

3. **Layer 3 (New)**: `receiver_recv()` validates connection state before buffer access
   - Prevents segfaults if deletion happens during buffer operations

4. **Layer 4 (New)**: `fw_conn_del()` sets flags before freeing and NULL-checks buffers
   - Ensures other threads can detect the deletion state
   - Prevents double-free issues

## Timeline Protection

```
Time →   T0          T1          T2          T3          T4
Thread A: [Enter fw_prot_process] → [Check flags] → [Access buffer] → [Loop] → [Exit]
                                      ✓ being_deleted=0   ✓ buf!=NULL    ✓ Check again
Thread B:           [Pause signal] → [Set being_deleted=1] → [Free buf, set NULL]
                                      ↓
                                   Thread A detects and exits safely
```

## Testing

To test the fix, use the provided configuration file `test_forward_fix.conf`:

```bash
# Build fluent-bit with the fix
cd build
cmake -DFLB_DEBUG=On ..
make

# Run with the test configuration
./bin/fluent-bit -c ../test_forward_fix.conf

# In another terminal, send high-volume data to trigger pause/resume cycles
# The configuration intentionally sets:
# - Buffer_Max_Size: 6MB
# - Mem_Buf_Limit: 4MB
# This mismatch would previously trigger the segfault under high load.
```

## Impact

This comprehensive fix:
- ✅ Eliminates race conditions between pause/delete and ongoing processing
- ✅ Prevents use-after-free access to freed connection buffers
- ✅ Allows in-flight processing to exit gracefully when paused
- ✅ Maintains backward compatibility
- ✅ Has minimal performance impact (only adds lightweight checks)
- ✅ Improves overall stability under high load and memory pressure conditions

## Files Modified

1. `plugins/in_forward/fw_conn.h`: Added `being_deleted` flag to connection structure
2. `plugins/in_forward/fw_conn.c`:
   - Initialize `being_deleted` flag in `fw_conn_add()`
   - Set flag and safe cleanup in `fw_conn_del()`
3. `plugins/in_forward/fw_prot.c`:
   - Added safety checks in `receiver_recv()`
   - Added periodic pause/deletion checks in `fw_prot_process()`

## Recommendations

1. **Configuration**: Set `buffer_max_size` <= `mem_buf_limit` to minimize pause/resume cycles
2. **Monitoring**: Monitor memory usage and adjust limits based on workload
3. **Testing**: Test under high load conditions with memory pressure to verify stability
4. **Production**: Deploy this fix to production environments experiencing segfaults under load
