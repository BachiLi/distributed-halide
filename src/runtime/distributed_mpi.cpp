#include "runtime_internal.h"

#include "HalideRuntime.h"

typedef int (*halide_task)(void *user_context, int, uint8_t *);

extern "C" {
extern int printf(const char *format, ...);
extern double ceil(double d);

typedef int MPI_Comm;
#define MPI_COMM_WORLD ((MPI_Comm)0x44000000)

typedef int MPI_Request;
#define MPI_REQUEST_NULL   ((MPI_Request)0x2c000000)

typedef int MPI_Datatype;
#define MPI_CHAR           ((MPI_Datatype)0x4c000101)
#define MPI_SIGNED_CHAR    ((MPI_Datatype)0x4c000118)
#define MPI_UNSIGNED_CHAR  ((MPI_Datatype)0x4c000102)
#define MPI_BYTE           ((MPI_Datatype)0x4c00010d)
#define MPI_WCHAR          ((MPI_Datatype)0x4c00040e)
#define MPI_SHORT          ((MPI_Datatype)0x4c000203)
#define MPI_UNSIGNED_SHORT ((MPI_Datatype)0x4c000204)
#define MPI_INT            ((MPI_Datatype)0x4c000405)
#define MPI_UNSIGNED       ((MPI_Datatype)0x4c000406)
#define MPI_LONG           ((MPI_Datatype)0x4c000807)
#define MPI_UNSIGNED_LONG  ((MPI_Datatype)0x4c000808)
#define MPI_FLOAT          ((MPI_Datatype)0x4c00040a)
#define MPI_DOUBLE         ((MPI_Datatype)0x4c00080b)
#define MPI_LONG_DOUBLE    ((MPI_Datatype)0x4c00100c)
#define MPI_LONG_LONG_INT  ((MPI_Datatype)0x4c000809)
#define MPI_UNSIGNED_LONG_LONG ((MPI_Datatype)0x4c000819)
#define MPI_LONG_LONG      MPI_LONG_LONG_INT

#define MPI_INT8_T            ((MPI_Datatype)0x4c000137)
#define MPI_INT16_T           ((MPI_Datatype)0x4c000238)
#define MPI_INT32_T           ((MPI_Datatype)0x4c000439)
#define MPI_INT64_T           ((MPI_Datatype)0x4c00083a)
#define MPI_UINT8_T           ((MPI_Datatype)0x4c00013b)
#define MPI_UINT16_T          ((MPI_Datatype)0x4c00023c)
#define MPI_UINT32_T          ((MPI_Datatype)0x4c00043d)
#define MPI_UINT64_T          ((MPI_Datatype)0x4c00083e)

#define MPI_ORDER_C              56

typedef struct MPI_Status {
    int count_lo;
    int count_hi_and_cancelled;
    int MPI_SOURCE;
    int MPI_TAG;
    int MPI_ERROR;
} MPI_Status;

#define MPI_SUCCESS          0      /* Successful return code */

extern int MPI_Comm_size(MPI_Comm, int *);
extern int MPI_Comm_rank(MPI_Comm, int *);
extern int MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm);
extern int MPI_Send(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
                    MPI_Comm comm);
extern int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
                     MPI_Comm comm, MPI_Request *request);
extern int MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
                    MPI_Comm comm, MPI_Status *status);
extern int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
                     MPI_Comm comm, MPI_Request *request);
extern int MPI_Wait(MPI_Request *request, MPI_Status *status);
extern int MPI_Waitall(int count, MPI_Request array_of_requests[], MPI_Status array_of_statuses[]);

extern int MPI_Type_commit(MPI_Datatype *datatype);
extern int MPI_Type_free(MPI_Datatype *datatype);
extern int MPI_Type_create_subarray(int ndims, const int array_of_sizes[],
                                    const int array_of_subsizes[], const int array_of_starts[],
                                    int order, MPI_Datatype oldtype, MPI_Datatype *newtype);

MPI_Comm HALIDE_MPI_COMM;

WEAK int halide_do_task(void *user_context, halide_task f, int idx,
                        uint8_t *closure);

extern void *malloc(size_t);
extern void *realloc(void *ptr, size_t size);
extern void *memset(void *s, int c, size_t n);
extern void free(void *ptr);
} // extern "C"

namespace Halide { namespace Runtime { namespace Internal {

int halide_num_processes;
bool halide_mpi_initialized = false;
bool trace_messages = true;

template <class T>
class SimpleVector {
public:
    SimpleVector() : _allocated_size(0), _idx(0), _data(NULL) {}

    void push_back(T t) {
        if (_idx == _allocated_size) {
            _allocated_size *= 2;
            _data = (T *)realloc(_data, _allocated_size * sizeof(T));
            halide_assert(NULL, _data != NULL);
        }
        _data[_idx++] = t;
    }

    void clear() {
        memset(_data, 0, _allocated_size * sizeof(T));
        _idx = 0;
    }

    void free() {
        free(_data);
        _idx = 0;
        _allocated_size = 0;
    }

    unsigned size() const {
        return _idx;
    }

    T operator[](unsigned i) {
        return *(_data + i);
    }

    T *data() {
        return _data;
    }

    const T *data() const {
        return _data;
    }

    void reserve(unsigned size) {
        halide_assert(NULL, size >= _allocated_size);
        _allocated_size = size;
        _data = (T *)realloc(_data, _allocated_size * sizeof(T));
        halide_assert(NULL, _data != NULL);
    }
private:
    unsigned _allocated_size;
    unsigned _idx;
    T *_data;
};

SimpleVector<MPI_Request> outstanding_receives;
SimpleVector<MPI_Request> outstanding_sends;
SimpleVector<MPI_Datatype> send_datatypes, recv_datatypes;

MPI_Datatype halide_to_mpi_type(halide_type_code_t type_code, int bits) {
    MPI_Datatype result = MPI_UNSIGNED_CHAR;
    switch (type_code) {
    case halide_type_int: {
        switch (bits) {
        case 8:
            return MPI_INT8_T;
        case 16:
            return MPI_INT16_T;
        case 32:
            return MPI_INT32_T;
        case 64:
            return MPI_INT64_T;
        default:
            halide_assert(NULL, false);
            break;
        }
    }
    case halide_type_uint: {
        switch (bits) {
        case 8:
            return MPI_UINT8_T;
        case 16:
            return MPI_UINT16_T;
        case 32:
            return MPI_UINT32_T;
        case 64:
            return MPI_UINT64_T;
        default:
            halide_assert(NULL, false);
            break;
        }
    }
    case halide_type_float: {
        switch (bits) {
        case 32:
            return MPI_FLOAT;
        case 64:
            return MPI_DOUBLE;
        default:
            halide_assert(NULL, false);
            break;
        }
    }
    case halide_type_handle:
    default:
        halide_assert(NULL, false);
        break;
    }
    return result;
}

WEAK void halide_initialize_mpi() {
    MPI_Comm_dup(MPI_COMM_WORLD, &HALIDE_MPI_COMM);
    MPI_Comm_size(HALIDE_MPI_COMM, &halide_num_processes);
    outstanding_receives.reserve(16);
    outstanding_sends.reserve(16);
    halide_mpi_initialized = true;
}

WEAK int default_do_task(void *user_context, halide_task f, int idx,
                        uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK int default_do_distr_for(void *user_context, halide_task f,
                            int min, int size, uint8_t *closure) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);
    int b = (int)ceil((double)size / halide_num_processes);
    int start = min + b*rank,
        finish = min + b*(rank+1);
    finish = finish <= size ? finish : size;
    for (int x = start; x < finish; x++) {
        int result = halide_do_task(user_context, f, x, closure);
        if (result) {
            return result;
        }
    }
    printf("After distr_for on rank %d over (%d,%d)\n", rank, start, finish);
    // Return zero if the job succeeded, otherwise return the exit
    // status.
    return 0;
}

WEAK int (*halide_custom_do_task)(void *user_context, halide_task, int, uint8_t *) = default_do_task;
WEAK int (*halide_custom_do_distr_for)(void *, halide_task, int, int, uint8_t *) = default_do_distr_for;

}}} // namespace Halide::Runtime::Internal

extern "C" {

WEAK int (*halide_set_custom_do_task(int (*f)(void *, halide_task, int, uint8_t *)))
          (void *, halide_task, int, uint8_t *) {
    int (*result)(void *, halide_task, int, uint8_t *) = halide_custom_do_task;
    halide_custom_do_task = f;
    return result;
}


WEAK int (*halide_set_custom_do_distr_for(int (*f)(void *, halide_task, int, int, uint8_t *)))
          (void *, halide_task, int, int, uint8_t *) {
    int (*result)(void *, halide_task, int, int, uint8_t *) = halide_custom_do_distr_for;
    halide_custom_do_distr_for = f;
    return result;
}

WEAK int halide_do_task(void *user_context, halide_task f, int idx,
                        uint8_t *closure) {
    return (*halide_custom_do_task)(user_context, f, idx, closure);
}

WEAK int halide_do_distr_for(void *user_context, int (*f)(void *, int, uint8_t *),
                           int min, int size, uint8_t *closure) {
  return (*halide_custom_do_distr_for)(user_context, f, min, size, closure);
}

WEAK int halide_do_distr_size() {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    return halide_num_processes;
}

WEAK int halide_do_distr_rank() {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);
    return rank;
}

WEAK int halide_do_distr_send(const void *buf, int count, int dest) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int tag = 0;
    if (trace_messages) {
        printf("[rank %d] Issuing send buf %p (buf[0]=%d), count %d, dest %d\n",
               rank, buf, *(int *)buf, count, dest);
    }
    int rc = MPI_Send(buf, count, MPI_UNSIGNED_CHAR, dest, tag, HALIDE_MPI_COMM);
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] send failed.\n", rank);
    }
    return rc;
}

WEAK int halide_do_distr_isend(const void *buf, int count, int dest) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int tag = 0;
    if (trace_messages) {
        printf("[rank %d] Issuing isend buf %p (buf[0]=%d), count %d, dest %d\n",
               rank, buf, *(int *)buf, count, dest);
    }
    MPI_Request req;
    int rc = MPI_Isend(buf, count, MPI_UNSIGNED_CHAR, dest, tag, HALIDE_MPI_COMM, &req);
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] isend failed.\n", rank);
    }
    return rc;
}

WEAK int halide_do_distr_isend_subarray(const void *buf, halide_type_code_t type_code, int type_bits, int ndims, int *sizes, int *subsizes, int *starts, int dest) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);
    if (trace_messages) {
        printf("[rank %d] Issuing isend_subarray to dest %d:\n", rank, dest);
        for (int i = 0; i < ndims; i++) {
            printf("    [rank %d] subsize[%d] = %d\n", rank, i, subsizes[i]);
        }
    }

    MPI_Datatype basetype = halide_to_mpi_type(type_code, type_bits);
    MPI_Datatype subarray;
    MPI_Type_create_subarray(ndims, sizes, subsizes, starts, MPI_ORDER_C, basetype, &subarray);
    MPI_Type_commit(&subarray);
    // Save the datatype so we can free it later.
    send_datatypes.push_back(subarray);

    MPI_Request req;
    int tag = 0;
    int rc = MPI_Isend(buf, 1, subarray, dest, tag, HALIDE_MPI_COMM, &req);
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] isend failed.\n", rank);
    }
    outstanding_sends.push_back(req);
    return rc;
}

WEAK int halide_do_distr_recv(void *buf, int count, int source) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int tag = 0;
    MPI_Status status;
    int rc = MPI_Recv(buf, count, MPI_UNSIGNED_CHAR, source, tag, HALIDE_MPI_COMM, &status);
    if (trace_messages) {
        printf("[rank %d] Received buf %p (buf[0]=%d), count %d, source %d\n",
               rank, buf, *(int *)buf, count, source);
    }
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] receive failed.\n", rank);
    }
    return rc;
}

WEAK int halide_do_distr_irecv(void *buf, int count, int source) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int tag = 0;
    MPI_Request req;
    if (trace_messages) {
        printf("[rank %d] Issuing irecv buf %p, count %d, source %d\n",
               rank, buf, count, source);
    }
    int rc = MPI_Irecv(buf, count, MPI_UNSIGNED_CHAR, source, tag, HALIDE_MPI_COMM, &req);
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] irecv failed.\n", rank);
    }
    outstanding_receives.push_back(req);
    return rc;
}

WEAK int halide_do_distr_irecv_subarray(void *buf, halide_type_code_t type_code, int type_bits, int ndims, int *sizes, int *subsizes, int *starts, int source) {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);
    if (trace_messages) {
        printf("[rank %d] Issuing irecv_subarray from source %d:\n", rank, source);
        for (int i = 0; i < ndims; i++) {
            printf("    [rank %d] subsize[%d] = %d\n", rank, i, subsizes[i]);
        }
    }

    MPI_Datatype basetype = halide_to_mpi_type(type_code, type_bits);
    MPI_Datatype subarray;
    MPI_Type_create_subarray(ndims, sizes, subsizes, starts, MPI_ORDER_C, basetype, &subarray);
    MPI_Type_commit(&subarray);
    // Save the datatype so we can free it later.
    recv_datatypes.push_back(subarray);

    MPI_Request req;
    int tag = 0;
    int rc = MPI_Irecv(buf, 1, subarray, source, tag, HALIDE_MPI_COMM, &req);
    if (rc != MPI_SUCCESS) {
        printf("[rank %d] irecv failed.\n", rank);
    }
    outstanding_receives.push_back(req);
    return rc;
}

WEAK int halide_do_distr_waitall_recvs() {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int count = outstanding_receives.size();
    if (trace_messages) {
        printf("[rank %d] Issuing recv waitall for %d irecvs\n", rank, count);
    }
    int rc = MPI_SUCCESS;
    if (count > 0) {
        MPI_Status stati[count];
        rc = MPI_Waitall(count, outstanding_receives.data(), stati);
        if (rc != MPI_SUCCESS) {
            printf("[rank %d] recv waitall failed.\n", rank);
        }
        outstanding_receives.clear();

        // Clean up datatypes
        for (unsigned i = 0; i < recv_datatypes.size(); i++) {
            MPI_Datatype dt = recv_datatypes[i];
            MPI_Type_free(&dt);
        }
        recv_datatypes.clear();
    }
    return rc;
}

WEAK int halide_do_distr_waitall_sends() {
    if (!halide_mpi_initialized) {
        halide_initialize_mpi();
    }
    int rank = 0;
    MPI_Comm_rank(HALIDE_MPI_COMM, &rank);

    int count = outstanding_sends.size();
    if (trace_messages) {
        printf("[rank %d] Issuing send waitall for %d isends\n", rank, count);
    }
    int rc = MPI_SUCCESS;
    if (count > 0) {
        MPI_Status stati[count];
        rc = MPI_Waitall(count, outstanding_sends.data(), stati);
        if (rc != MPI_SUCCESS) {
            printf("[rank %d] send waitall failed.\n", rank);
        }
        outstanding_sends.clear();

        // Clean up datatypes
        for (unsigned i = 0; i < send_datatypes.size(); i++) {
            MPI_Datatype dt = send_datatypes[i];
            MPI_Type_free(&dt);
        }
        send_datatypes.clear();
    }
    return rc;
}

} // extern "C"
