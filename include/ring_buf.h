#ifndef __ring_buf_h
#define __ring_buf_h

#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/*------------------------------------------------------------------------------
 * Max Guo
 * December 2, 2011
 * Ring Buffer implementation
 *
 * leandrotsampa
 * February 14, 2018
 * pthread, timeout and bool implementation
 *----------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
 * USAGE: - Ring Buffer header file, can be used wherever, whenever, however
 *----------------------------------------------------------------------------*/

typedef struct {
    char *buf_ptr;
    int buf_size;
    int free_slots;
    int read_ptr;
    int write_ptr;
    pthread_mutex_t lock;
} ring_buffer;

/*------------------------------------------------------------------------------
 * create_buf() method - initializes circular buffer to a certain size
 * inputs:
 *     int size - size of circular buffer
 * returns:
 *     ring_buffer* - pointer to created circular buffer
 *----------------------------------------------------------------------------*/
ring_buffer *create_buf(int size)
{
    ring_buffer *buf = (ring_buffer *)malloc(sizeof(ring_buffer));
    buf->buf_ptr = (char *)calloc(size, sizeof(char));
    buf->buf_size = size;
    buf->free_slots = size;
    buf->read_ptr = 0;
    buf->write_ptr = 0;
    pthread_mutex_init(&buf->lock, NULL);
    return buf;
}

/*------------------------------------------------------------------------------
 * get_max_read_size() method - returns how much data can be read
 * inputs:
 *     ring_buffer *buf - buffer to check
 * returns:
 *     int - maximum size of data that can be read
 *----------------------------------------------------------------------------*/
int get_max_read_size(ring_buffer *buf)
{
    int size = 0;

    if (buf->buf_ptr)
    {
        pthread_mutex_lock(&buf->lock);

        if (buf->read_ptr == buf->write_ptr)
        {
            if (!(buf->free_slots == buf->buf_size))
                size = buf->buf_size;
        }
        else if (buf->read_ptr < buf->write_ptr)
            size = buf->write_ptr - buf->read_ptr;
        else if (buf->read_ptr > buf->write_ptr)
            size = buf->buf_size - buf->read_ptr + buf->write_ptr;

        pthread_mutex_unlock(&buf->lock);
    }

    return size;
}

/*------------------------------------------------------------------------------
 * get_max_write_size() method - returns how much space can be written to
 * inputs:
 *     ring_buffer *buf - buffer to check
 * returns:
 *     int - maximum amount of space that can be written to
 *----------------------------------------------------------------------------*/
int get_max_write_size(ring_buffer *buf)
{
    int size = 0;

    if (buf->buf_ptr)
    {
        pthread_mutex_lock(&buf->lock);

        if (buf->read_ptr == buf->write_ptr)
        {
            if (buf->free_slots == buf->buf_size)
                size = buf->buf_size;
        }
        else if (buf->write_ptr < buf->read_ptr)
            size = buf->read_ptr - buf->write_ptr;
        else if (buf->write_ptr > buf->read_ptr)
            size = buf->buf_size - buf->write_ptr + buf->read_ptr;

        pthread_mutex_unlock(&buf->lock);
    }

    return size;
}

/*------------------------------------------------------------------------------
 * write_to_buf() method - writes data to circular buffer, does not overwrite
 * inputs:
 *     ring_buffer *buf - buffer to write to
 *     char *data - data to write
 *     int length - length of data
 * returns:
 *     bool - false on failed write
 *            true on successful write
 *----------------------------------------------------------------------------*/
bool write_to_buf(ring_buffer *buf, char *data, int length)
{
    if (length <= get_max_write_size(buf))
    {
        pthread_mutex_lock(&buf->lock);

        if (buf->write_ptr + length < buf->buf_size) // no wrap-around
        {
            memcpy(&buf->buf_ptr[buf->write_ptr], data, length);
            buf->write_ptr += length;
        }
        else // data wraps around
        {
            int first_size = buf->buf_size - buf->write_ptr;
            int second_size = length - first_size;
            memcpy(&buf->buf_ptr[buf->write_ptr], data, first_size);
            memcpy(&buf->buf_ptr[0], &data[first_size], second_size);
            buf->write_ptr = second_size;
        }

        pthread_mutex_unlock(&buf->lock);
        return true;
    }

    return false;
}

/*------------------------------------------------------------------------------
 * write_to_buf_timeout() method - writes data to circular buffer, does not overwrite
 * inputs:
 *     ring_buffer *buf - buffer to write to
 *     char *data - data to write
 *     int length - length of data
 *     int timeout- time in seconds to still try
 * returns:
 *     bool - false on failed write
 *            true on successful write
 *----------------------------------------------------------------------------*/
bool write_to_buf_timeout(ring_buffer *buf, char *data, int length, int timeout)
{
    time_t seconds = timeout;
    time_t start = time(NULL);
    time_t endwait = start + seconds;

    while (start < endwait)
    {
        if (length <= get_max_write_size(buf))
        {
            pthread_mutex_lock(&buf->lock);

            if (buf->write_ptr + length < buf->buf_size) // no wrap-around
            {
                memcpy(&buf->buf_ptr[buf->write_ptr], data, length);
                buf->write_ptr += length;
            }
            else // data wraps around
            {
                int first_size = buf->buf_size - buf->write_ptr;
                int second_size = length - first_size;
                memcpy(&buf->buf_ptr[buf->write_ptr], data, first_size);
                memcpy(&buf->buf_ptr[0], &data[first_size], second_size);
                buf->write_ptr = second_size;
            }

            pthread_mutex_unlock(&buf->lock);
            return true;
        }

        sleep(1); /* To prevent high CPU usage. */
        start = time(NULL);
    }

    return false;
}

/*------------------------------------------------------------------------------
 * write_to_buf_over() method - writes data to circular buffer, overwrites
 * inputs:
 *     ring_buffer *buf - buffer to write to
 *     char *data - data to write
 *     int length - length of data
 * returns:
 *     void - no return value
 *----------------------------------------------------------------------------*/
void write_to_buf_over(ring_buffer *buf, char *data, int length)
{
    pthread_mutex_lock(&buf->lock);

    if (buf->write_ptr + length < buf->buf_size) // no wrap-around
    {
        memcpy(&buf->buf_ptr[buf->write_ptr], data, length);
        buf->write_ptr += length;
    }
    else // data wraps around
    {
        int first_size = buf->buf_size - buf->write_ptr;
        int second_size = length - first_size;
        memcpy(&buf->buf_ptr[buf->write_ptr], data, first_size);
        memcpy(&buf->buf_ptr[0], &data[first_size], second_size);
        buf->write_ptr = second_size;
    }

    if (buf->free_slots == 0)
        buf->read_ptr = buf->write_ptr;

    pthread_mutex_unlock(&buf->lock);

    if (length <= get_max_write_size(buf))
        buf->free_slots -= length;
}

/*------------------------------------------------------------------------------
 * read_buf() method - read data from circular buffer
 * inputs:
 *     ring_buffer *buf - buffer to read from
 *     char *store - read data into this storage
 *     int length  - length of data
 * returns:
 *     bool - false on failed read
 *            true on successful read
 *----------------------------------------------------------------------------*/
bool read_buf(ring_buffer *buf, char *store, int length)
{
    if (length <= get_max_read_size(buf))
    {
        pthread_mutex_lock(&buf->lock);

        if (buf->read_ptr + length < buf->buf_size) // no wrap-around
        {
            memcpy(store, &buf->buf_ptr[buf->read_ptr], length);
            buf->read_ptr += length;
        }
        else // data wraps around
        {
            int first_size = buf->buf_size - buf->read_ptr;
            int second_size = length - first_size;
            memcpy(store, &buf->buf_ptr[buf->read_ptr], first_size);
            memcpy(&store[first_size], &buf->buf_ptr[0], second_size);
            buf->read_ptr = second_size;
        }

        pthread_mutex_unlock(&buf->lock);
        return true;
    }

    return false;
}

/*------------------------------------------------------------------------------
 * read_buf_timeout() method - read data from circular buffer
 * inputs:
 *     ring_buffer *buf - buffer to read from
 *     char *store - read data into this storage
 *     int length  - length of data
 *     int timeout - time in seconds to still try
 * returns:
 *     bool - false on failed read
 *            true on successful read
 *----------------------------------------------------------------------------*/
bool read_buf_timeout(ring_buffer *buf, char *store, int length, int timeout)
{
    time_t seconds = timeout;
    time_t start = time(NULL);
    time_t endwait = start + seconds;

    while (start < endwait)
    {
        if (length <= get_max_read_size(buf))
        {
            pthread_mutex_lock(&buf->lock);

            if (buf->read_ptr + length < buf->buf_size) // no wrap-around
            {
                memcpy(store, &buf->buf_ptr[buf->read_ptr], length);
                buf->read_ptr += length;
            }
            else // data wraps around
            {
                int first_size = buf->buf_size - buf->read_ptr;
                int second_size = length - first_size;
                memcpy(store, &buf->buf_ptr[buf->read_ptr], first_size);
                memcpy(&store[first_size], &buf->buf_ptr[0], second_size);
                buf->read_ptr = second_size;
            }

            pthread_mutex_unlock(&buf->lock);
            return true;
        }

        sleep(1); /* To prevent high CPU usage. */
        start = time(NULL);
    }

    return false;
}

/*------------------------------------------------------------------------------
 * free_buf() method - cleanup method, frees buffer memory
 * inputs:
 *     ring_buffer *buf - buffer to cleanup
 * returns:
 *     void - no return value
 *----------------------------------------------------------------------------*/
void free_buf(ring_buffer *buf)
{
    if(buf->buf_ptr)
        free(buf->buf_ptr);
    buf->buf_ptr = NULL;
    buf->buf_size = 0;
    buf->read_ptr = 0;
    buf->write_ptr = 0;
    pthread_mutex_destroy(&buf->lock);
}
#endif