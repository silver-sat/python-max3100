/*
 * max3100_module.c - Python bindings for Linux serial communication 
 *                    using the MAX3100 and SPI through spidev
 *
 * MIT License
 *
 * Copyright (C) 2022 Nathan Edwards <edwardsnj@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <Python.h>
#include "structmember.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>

#define _VERSION_ "0.1"
#define SPIDEV_MAXPATH 4096

#define BLOCK_SIZE_CONTROL_FILE "/sys/module/spidev/parameters/bufsiz"
// The xfwr3 function attempts to use large blocks if /sys/module/spidev/parameters/bufsiz setting allows it.
// However where we cannot get a value from that file, we fall back to this safe default.
#define XFER3_DEFAULT_BLOCK_SIZE SPIDEV_MAXPATH
// Largest block size for xfer3 - even if /sys/module/spidev/parameters/bufsiz allows bigger
// blocks, we won't go above this value. As I understand, DMA is not used for anything bigger so why bother.
#define XFER3_MAX_BLOCK_SIZE 65535

// MAX3100 16-bit constants
//
// Commands.
#define MAX3100_CMD_WRITE_CONF      0b1100000000000000
#define MAX3100_CMD_READ_CONF       0b0100000000000000
#define MAX3100_CMD_WRITE_DATA      0b1000000000000000
#define MAX3100_CMD_READ_DATA       0b0000000000000000
    
// Configuration.
#define MAX3100_CONF_R              0b1000000000000000
#define MAX3100_CONF_T              0b0100000000000000
#define MAX3100_CONF_RM             0b0000010000000000

// Baud rates for clock multiplier x1.
#define MAX3100_CONF_BAUD_X1_115200 0b0000000000000000
#define MAX3100_CONF_BAUD_X1_57600  0b0000000000000001
#define MAX3100_CONF_BAUD_X1_38400  0b0000000000001000
#define MAX3100_CONF_BAUD_X1_19200  0b0000000000001001
#define MAX3100_CONF_BAUD_X1_9600   0b0000000000001010
#define MAX3100_CONF_BAUD_X1_4800   0b0000000000001011
#define MAX3100_CONF_BAUD_X1_2400   0b0000000000001100
#define MAX3100_CONF_BAUD_X1_1200   0b0000000000001101
#define MAX3100_CONF_BAUD_X1_600    0b0000000000001110
#define MAX3100_CONF_BAUD_X1_300    0b0000000000001111
    
// Baud rates for clock multiplier x2.
#define MAX3100_CONF_BAUD_X2_230400 0b0000000000000000
#define MAX3100_CONF_BAUD_X2_115200 0b0000000000000001
#define MAX3100_CONF_BAUD_X2_57600  0b0000000000000010
#define MAX3100_CONF_BAUD_X2_38400  0b0000000000001001
#define MAX3100_CONF_BAUD_X2_19200  0b0000000000001010
#define MAX3100_CONF_BAUD_X2_9600   0b0000000000001011
#define MAX3100_CONF_BAUD_X2_4800   0b0000000000001100
#define MAX3100_CONF_BAUD_X2_2400   0b0000000000001101
#define MAX3100_CONF_BAUD_X2_1200   0b0000000000001110
#define MAX3100_CONF_BAUD_X2_600    0b0000000000001111

#if PY_MAJOR_VERSION < 3
#define PyLong_AS_LONG(val) PyInt_AS_LONG(val)
#define PyLong_AsLong(val) PyInt_AsLong(val)
#endif

// Macros needed for Python 3
#ifndef PyInt_Check
#define PyInt_Check			PyLong_Check
#define PyInt_FromLong	PyLong_FromLong
#define PyInt_AsLong		PyLong_AsLong
#define PyInt_Type			PyLong_Type
#endif

// Maximum block size for xfer3
// Initialised once by get_xfer3_block_size
uint32_t xfer3_block_size = 0;

// Read maximum block size from the /sys/module/spidev/parameters/bufsiz
// In case of any problems reading the number, we fall back to XFER3_DEFAULT_BLOCK_SIZE.
// If number is read ok but it exceeds the XFER3_MAX_BLOCK_SIZE, it will be capped to that value.
// The value is read and cached on the first invocation. Following invocations just return the cached one.
uint32_t get_xfer3_block_size(void) {
	int value;

	// If value was already initialised, just use it
	if (xfer3_block_size != 0) {
		return xfer3_block_size;
	}

	// Start with the default
	xfer3_block_size = XFER3_DEFAULT_BLOCK_SIZE;

	FILE *file = fopen(BLOCK_SIZE_CONTROL_FILE,"r");
	if (file != NULL) {
		if (fscanf(file, "%d", &value) == 1 && value > 0) {
			if (value <= XFER3_MAX_BLOCK_SIZE) {
				xfer3_block_size = value;
			} else {
				xfer3_block_size = XFER3_MAX_BLOCK_SIZE;
			}
		}
		fclose(file);
	}

	return xfer3_block_size;
}

PyDoc_STRVAR(MAX3100_module_doc,
	"This module defines an object type that allows serial communication\n"
	"via the MAX3100 chip and SPI\n"
	"on hosts running the Linux kernel. The host kernel must have SPI\n"
	"support and SPI device interface support.\n"
	"All of these can be either built-in to the kernel, or loaded from\n"
	"modules.\n"
	"\n"
	"Because the SPI device interface is opened R/W, users of this\n"
	"module usually must have root permissions.\n");

typedef struct {
	PyObject_HEAD

	int fd;	/* open file descriptor: /dev/spidevX.Y */
	uint8_t mode;	/* current SPI mode */
	uint8_t bits_per_word;	/* current SPI bits per word setting */
	uint32_t max_speed_hz;	/* current SPI max speed setting in Hz */
	uint8_t read0;	/* read 0 bytes after transfer to lwoer CS if SPI_CS_HIGH */
} MAX3100_Object;

static PyObject *
MAX3100_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	MAX3100_Object *self;
	if ((self = (MAX3100_Object *)type->tp_alloc(type, 0)) == NULL)
		return NULL;

	self->fd = -1;
	self->mode = 0;
	self->bits_per_word = 0;
	self->max_speed_hz = 0;
	
	Py_INCREF(self);
	return (PyObject *)self;
}

PyDoc_STRVAR(MAX3100_close_doc,
	"close()\n\n"
	"Disconnects the object from the interface.\n");

static PyObject *
MAX3100_close(MAX3100_Object *self)
{
	if ((self->fd != -1) && (close(self->fd) == -1)) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	self->fd = -1;
	self->mode = 0;
	self->bits_per_word = 0;
	self->max_speed_hz = 0;

	Py_INCREF(Py_None);
	return Py_None;
}

static void
MAX3100_dealloc(MAX3100_Object *self)
{
	PyObject *ref = MAX3100_close(self);
	Py_XDECREF(ref);

	Py_TYPE(self)->tp_free((PyObject *)self);
}

static char *wrmsg_list0 = "Empty argument list.";
static char *wrmsg_listmax = "Argument list size exceeds %d bytes.";
static char *wrmsg_val = "Non-Int/Long value in arguments: %x.";
static char *wrmsg_oom = "Out of memory.";


PyDoc_STRVAR(MAX3100_write_doc,
	"write([values]) -> None\n\n"
	"Write bytes to SPI device.\n");

static PyObject *
MAX3100_writebytes(MAX3100_Object *self, PyObject *args)
{
	int		status;
	uint16_t	ii, len;
	uint8_t	buf[SPIDEV_MAXPATH];
	PyObject	*obj;
	PyObject	*seq;
	char	wrmsg_text[4096];

	if (!PyArg_ParseTuple(args, "O:write", &obj))
		return NULL;

	seq = PySequence_Fast(obj, "expected a sequence");
	len = PySequence_Fast_GET_SIZE(seq);
	if (!seq || len <= 0) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	if (len > SPIDEV_MAXPATH) {
		snprintf(wrmsg_text, sizeof (wrmsg_text) - 1, wrmsg_listmax, SPIDEV_MAXPATH);
		PyErr_SetString(PyExc_OverflowError, wrmsg_text);
		return NULL;
	}

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PySequence_Fast_GET_ITEM(seq, ii);
#if PY_MAJOR_VERSION < 3
		if (PyInt_Check(val)) {
			buf[ii] = (__u8)PyInt_AS_LONG(val);
		} else
#endif
		{
			if (PyLong_Check(val)) {
				buf[ii] = (__u8)PyLong_AS_LONG(val);
			} else {
				snprintf(wrmsg_text, sizeof (wrmsg_text) - 1, wrmsg_val, val);
				PyErr_SetString(PyExc_TypeError, wrmsg_text);
				return NULL;
			}
		}
	}

	Py_DECREF(seq);

	status = write(self->fd, &buf[0], len);

	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	if (status != len) {
		perror("short write");
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(MAX3100_read_doc,
	"read(len) -> [values]\n\n"
	"Read len bytes from SPI device.\n");

static PyObject *
MAX3100_readbytes(MAX3100_Object *self, PyObject *args)
{
	uint8_t	rxbuf[SPIDEV_MAXPATH];
	int		status, len, ii;
	PyObject	*list;

	if (!PyArg_ParseTuple(args, "i:read", &len))
		return NULL;

	/* read at least 1 byte, no more than SPIDEV_MAXPATH */
	if (len < 1)
		len = 1;
	else if ((unsigned)len > sizeof(rxbuf))
		len = sizeof(rxbuf);

	memset(rxbuf, 0, sizeof rxbuf);
	status = read(self->fd, &rxbuf[0], len);

	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	if (status != len) {
		perror("short read");
		return NULL;
	}

	list = PyList_New(len);

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PyLong_FromLong((long)rxbuf[ii]);
		PyList_SET_ITEM(list, ii, val);  // Steals reference, no need to Py_DECREF(val)
	}

	return list;
}

static PyObject *
MAX3100_writebytes2_buffer(MAX3100_Object *self, Py_buffer *buffer)
{
	int		status;
	Py_ssize_t	remain, block_size, block_start, spi_max_block;

	spi_max_block = get_xfer3_block_size();

	block_start = 0;
	remain = buffer->len;
	while (block_start < buffer->len) {
		block_size = (remain < spi_max_block) ? remain : spi_max_block;

		Py_BEGIN_ALLOW_THREADS
		status = write(self->fd, buffer->buf + block_start, block_size);
		Py_END_ALLOW_THREADS

		if (status < 0) {
			PyErr_SetFromErrno(PyExc_IOError);
			return NULL;
		}

		if (status != block_size) {
			perror("short write");
			return NULL;
		}

		block_start += block_size;
		remain -= block_size;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
MAX3100_writebytes2_seq_internal(MAX3100_Object *self, PyObject *seq, Py_ssize_t len, uint8_t *buf, Py_ssize_t bufsize)
{
	int		status;
	Py_ssize_t	ii, jj, remain, block_size;
	char	wrmsg_text[4096];

	remain = len;
	jj = 0;
	while (remain > 0) {
		block_size = (remain < bufsize) ? remain : bufsize;

		for (ii = 0; ii < block_size; ii++, jj++) {
			PyObject *val = PySequence_Fast_GET_ITEM(seq, jj);
#if PY_MAJOR_VERSION < 3
			if (PyInt_Check(val)) {
				buf[ii] = (__u8)PyInt_AS_LONG(val);
			} else
#endif
			{
				if (PyLong_Check(val)) {
					buf[ii] = (__u8)PyLong_AS_LONG(val);
				} else {
					snprintf(wrmsg_text, sizeof (wrmsg_text) - 1, wrmsg_val, val);
					PyErr_SetString(PyExc_TypeError, wrmsg_text);
					return NULL;
				}
			}
		}

		Py_BEGIN_ALLOW_THREADS
		status = write(self->fd, buf, block_size);
		Py_END_ALLOW_THREADS

		if (status < 0) {
			PyErr_SetFromErrno(PyExc_IOError);
			return NULL;
		}

		if (status != block_size) {
			perror("short write");
			return NULL;
		}

		remain -= block_size;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

// In writebytes2 we try to avoild doing malloc/free on each tiny block.
// So for any transfer below this size we will use on-stack local buffer instead of allocating one on the heap.
#define SMALL_BUFFER_SIZE 128

static PyObject *
MAX3100_writebytes2_seq(MAX3100_Object *self, PyObject *seq)
{
	Py_ssize_t	len, bufsize, spi_max_block;
	PyObject	*result = NULL;

	len = PySequence_Fast_GET_SIZE(seq);
	if (len <= 0) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	spi_max_block = get_xfer3_block_size();

	bufsize = (len < spi_max_block) ? len : spi_max_block;

	if (bufsize <= SMALL_BUFFER_SIZE) {
		// The data size is very small so we can avoid malloc/free completely
		// by using a small local buffer instead
		uint8_t buf[SMALL_BUFFER_SIZE];
		result = MAX3100_writebytes2_seq_internal(self, seq, len, buf, SMALL_BUFFER_SIZE);
	} else {
		// Large data, need to allocate buffer on heap
		uint8_t	*buf;
		Py_BEGIN_ALLOW_THREADS
		buf = malloc(sizeof(__u8) * bufsize);
		Py_END_ALLOW_THREADS

		if (!buf) {
			PyErr_SetString(PyExc_OverflowError, wrmsg_oom);
			return NULL;
		}

		result = MAX3100_writebytes2_seq_internal(self, seq, len, buf, bufsize);

		Py_BEGIN_ALLOW_THREADS
		free(buf);
		Py_END_ALLOW_THREADS
	}

	return result;
}

PyDoc_STRVAR(MAX3100_writebytes2_doc,
	"writebytes2([values]) -> None\n\n"
	"Write bytes to SPI device.\n"
	"values must be a list or buffer.\n");

static PyObject *
MAX3100_writebytes2(MAX3100_Object *self, PyObject *args)
{
	PyObject	*obj, *seq;;
	PyObject	*result = NULL;

	if (!PyArg_ParseTuple(args, "O:writebytes2", &obj)) {
		return NULL;
	}

	// Try using buffer protocol if object supports it.
	if (PyObject_CheckBuffer(obj) && 1) {
		Py_buffer	buffer;
		if (PyObject_GetBuffer(obj, &buffer, PyBUF_SIMPLE) != -1) {
			result = MAX3100_writebytes2_buffer(self, &buffer);
			PyBuffer_Release(&buffer);
			return result;
		}
	}


	// Otherwise, fall back to sequence protocol
	seq = PySequence_Fast(obj, "expected a sequence");
	if (seq == NULL) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	result = MAX3100_writebytes2_seq(self, seq);

	Py_DECREF(seq);

	return result;

}

PyDoc_STRVAR(MAX3100_xfer_doc,
	"xfer([values]) -> [values]\n\n"
	"Perform SPI transaction.\n"
	"CS will be released and reactivated between blocks.\n"
	"delay specifies delay in usec between blocks.\n");

static PyObject *
MAX3100_xfer(MAX3100_Object *self, PyObject *args)
{
	uint16_t ii, len;
	int status;
	uint16_t delay_usecs = 0;
	uint32_t speed_hz = 0;
	uint8_t bits_per_word = 0;
	PyObject *obj;
	PyObject *seq;
#ifdef SPIDEV_SINGLE
	struct spi_ioc_transfer *xferptr;
	memset(&xferptr, 0, sizeof(xferptr));
#else
	struct spi_ioc_transfer xfer;
	memset(&xfer, 0, sizeof(xfer));
#endif
	uint8_t *txbuf, *rxbuf;
	char	wrmsg_text[4096];

	if (!PyArg_ParseTuple(args, "O|IHB:xfer", &obj, &speed_hz, &delay_usecs, &bits_per_word))
		return NULL;

	seq = PySequence_Fast(obj, "expected a sequence");
	if (!seq) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	len = PySequence_Fast_GET_SIZE(seq);
	if (len <= 0) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	if (len > SPIDEV_MAXPATH) {
		snprintf(wrmsg_text, sizeof(wrmsg_text) - 1, wrmsg_listmax, SPIDEV_MAXPATH);
		PyErr_SetString(PyExc_OverflowError, wrmsg_text);
		Py_DECREF(seq);
		return NULL;
	}

	txbuf = malloc(sizeof(__u8) * len);
	rxbuf = malloc(sizeof(__u8) * len);

#ifdef SPIDEV_SINGLE
	xferptr = (struct spi_ioc_transfer*) malloc(sizeof(struct spi_ioc_transfer) * len);

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PySequence_Fast_GET_ITEM(seq, ii);
#if PY_MAJOR_VERSION < 3
		if (PyInt_Check(val)) {
			txbuf[ii] = (__u8)PyInt_AS_LONG(val);
		} else
#endif
		{
			if (PyLong_Check(val)) {
				txbuf[ii] = (__u8)PyLong_AS_LONG(val);
			} else {
				snprintf(wrmsg_text, sizeof(wrmsg_text) - 1, wrmsg_val, val);
				PyErr_SetString(PyExc_TypeError, wrmsg_text);
				free(xferptr);
				free(txbuf);
				free(rxbuf);
				Py_DECREF(seq);
				return NULL;
			}
		}
		xferptr[ii].tx_buf = (unsigned long)&txbuf[ii];
		xferptr[ii].rx_buf = (unsigned long)&rxbuf[ii];
		xferptr[ii].len = 1;
		xferptr[ii].delay_usecs = delay;
		xferptr[ii].speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
		xferptr[ii].bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;
#ifdef SPI_IOC_WR_MODE32
		xferptr[ii].tx_nbits = 0;
#endif
#ifdef SPI_IOC_RD_MODE32
		xferptr[ii].rx_nbits = 0;
#endif
	}

	status = ioctl(self->fd, SPI_IOC_MESSAGE(len), xferptr);
	free(xferptr);
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free(txbuf);
		free(rxbuf);
		Py_DECREF(seq);
		return NULL;
	}
#else
	for (ii = 0; ii < len; ii++) {
		PyObject *val = PySequence_Fast_GET_ITEM(seq, ii);
#if PY_MAJOR_VERSION < 3
		if (PyInt_Check(val)) {
			txbuf[ii] = (__u8)PyInt_AS_LONG(val);
		} else
#endif
		{
			if (PyLong_Check(val)) {
				txbuf[ii] = (__u8)PyLong_AS_LONG(val);
			} else {
				snprintf(wrmsg_text, sizeof(wrmsg_text) - 1, wrmsg_val, val);
				PyErr_SetString(PyExc_TypeError, wrmsg_text);
				free(txbuf);
				free(rxbuf);
				Py_DECREF(seq);
				return NULL;
			}
		}
	}

	if (PyTuple_Check(obj)) {
		Py_DECREF(seq);
		seq = PySequence_List(obj);
	}

	xfer.tx_buf = (unsigned long)txbuf;
	xfer.rx_buf = (unsigned long)rxbuf;
	xfer.len = len;
	xfer.delay_usecs = delay_usecs;
	xfer.speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
	xfer.bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;
#ifdef SPI_IOC_WR_MODE32
	xfer.tx_nbits = 0;
#endif
#ifdef SPI_IOC_RD_MODE32
	xfer.rx_nbits = 0;
#endif

	status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free(txbuf);
		free(rxbuf);
		Py_DECREF(seq);
		return NULL;
	}
#endif

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PyLong_FromLong((long)rxbuf[ii]);
		PySequence_SetItem(seq, ii, val);
		Py_DECREF(val); // PySequence_SetItem does not steal reference, must Py_DECREF(val)
	}

	// WA:
	// in CS_HIGH mode CS isn't pulled to low after transfer, but after read
	// reading 0 bytes doesnt matter but brings cs down
	// tomdean:
	// Stop generating an extra CS except in mode CS_HOGH
	if (self->read0 && (self->mode & SPI_CS_HIGH)) status = read(self->fd, &rxbuf[0], 0);

	free(txbuf);
	free(rxbuf);

	if (PyTuple_Check(obj)) {
		PyObject *old = seq;
		seq = PySequence_Tuple(seq);
		Py_DECREF(old);
	}

	return seq;
}


PyDoc_STRVAR(MAX3100_xfer2_doc,
	"xfer2([values]) -> [values]\n\n"
	"Perform SPI transaction.\n"
	"CS will be held active between blocks.\n");

static PyObject *
MAX3100_xfer2(MAX3100_Object *self, PyObject *args)
{
	int status;
	uint16_t delay_usecs = 0;
	uint32_t speed_hz = 0;
	uint8_t bits_per_word = 0;
	uint16_t ii, len;
	PyObject *obj;
	PyObject *seq;
	struct spi_ioc_transfer xfer;
	Py_BEGIN_ALLOW_THREADS
	memset(&xfer, 0, sizeof(xfer));
	Py_END_ALLOW_THREADS
	uint8_t *txbuf, *rxbuf;
	char	wrmsg_text[4096];

	if (!PyArg_ParseTuple(args, "O|IHB:xfer2", &obj, &speed_hz, &delay_usecs, &bits_per_word))
		return NULL;

	seq = PySequence_Fast(obj, "expected a sequence");
	if (!seq) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	len = PySequence_Fast_GET_SIZE(seq);
	if (len <= 0) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	if (len > SPIDEV_MAXPATH) {
		snprintf(wrmsg_text, sizeof(wrmsg_text) - 1, wrmsg_listmax, SPIDEV_MAXPATH);
		PyErr_SetString(PyExc_OverflowError, wrmsg_text);
		Py_DECREF(seq);
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS
	txbuf = malloc(sizeof(__u8) * len);
	rxbuf = malloc(sizeof(__u8) * len);
	Py_END_ALLOW_THREADS

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PySequence_Fast_GET_ITEM(seq, ii);
#if PY_MAJOR_VERSION < 3
		if (PyInt_Check(val)) {
			txbuf[ii] = (__u8)PyInt_AS_LONG(val);
		} else
#endif
		{
			if (PyLong_Check(val)) {
				txbuf[ii] = (__u8)PyLong_AS_LONG(val);
			} else {
				snprintf(wrmsg_text, sizeof (wrmsg_text) - 1, wrmsg_val, val);
				PyErr_SetString(PyExc_TypeError, wrmsg_text);
				free(txbuf);
				free(rxbuf);
				Py_DECREF(seq);
				return NULL;
			}
		}
	}

	if (PyTuple_Check(obj)) {
		Py_DECREF(seq);
		seq = PySequence_List(obj);
	}

	Py_BEGIN_ALLOW_THREADS
	xfer.tx_buf = (unsigned long)txbuf;
	xfer.rx_buf = (unsigned long)rxbuf;
	xfer.len = len;
	xfer.delay_usecs = delay_usecs;
	xfer.speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
	xfer.bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;

	status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
	Py_END_ALLOW_THREADS
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free(txbuf);
		free(rxbuf);
		Py_DECREF(seq);
		return NULL;
	}

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PyLong_FromLong((long)rxbuf[ii]);
		PySequence_SetItem(seq, ii, val);
		Py_DECREF(val); // PySequence_SetItem does not steal reference, must Py_DECREF(val)
	}
	// WA:
	// in CS_HIGH mode CS isnt pulled to low after transfer
	// reading 0 bytes doesn't really matter but brings CS down
	// tomdean:
	// Stop generating an extra CS except in mode CS_HOGH
	if (self->read0 && (self->mode & SPI_CS_HIGH)) status = read(self->fd, &rxbuf[0], 0);

	Py_BEGIN_ALLOW_THREADS
	free(txbuf);
	free(rxbuf);
	Py_END_ALLOW_THREADS


	if (PyTuple_Check(obj)) {
		PyObject *old = seq;
		seq = PySequence_Tuple(seq);
		Py_DECREF(old);
	}

	return seq;
}

PyDoc_STRVAR(MAX3100_xfer3_doc,
	"xfer3([values]) -> [values]\n\n"
	"Perform SPI transaction. Accepts input of arbitrary size.\n"
	"Large blocks will be send as multiple transactions\n"
	"CS will be held active between blocks.\n");

static PyObject *
MAX3100_xfer3(MAX3100_Object *self, PyObject *args)
{
	int status;
	uint16_t delay_usecs = 0;
	uint32_t speed_hz = 0;
	uint8_t bits_per_word = 0;
	Py_ssize_t ii, jj, len, block_size, block_start, bufsize;
	PyObject *obj;
	PyObject *seq;
	PyObject *rx_tuple;
	struct spi_ioc_transfer xfer;
	Py_BEGIN_ALLOW_THREADS
	memset(&xfer, 0, sizeof(xfer));
	Py_END_ALLOW_THREADS
	uint8_t *txbuf, *rxbuf;
	char	wrmsg_text[4096];

	if (!PyArg_ParseTuple(args, "O|IHB:xfer3", &obj, &speed_hz, &delay_usecs, &bits_per_word))
		return NULL;

	seq = PySequence_Fast(obj, "expected a sequence");
	if (!seq) {
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	len = PySequence_Fast_GET_SIZE(seq);
	if (len <= 0) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_TypeError, wrmsg_list0);
		return NULL;
	}

	bufsize = get_xfer3_block_size();
	if (bufsize > len) {
		bufsize = len;
	}

	rx_tuple = PyTuple_New(len);
	if (!rx_tuple) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_OverflowError, wrmsg_oom);
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS
	// Allocate tx and rx buffers immediately releasing them if any allocation fails
	if ((txbuf = malloc(sizeof(__u8) * bufsize)) != NULL) {
		if ((rxbuf = malloc(sizeof(__u8) * bufsize)) != NULL) {
			// All good, both buffers allocated
		} else {
			// rxbuf allocation failed while txbuf succeeded
			free(txbuf);
			txbuf = NULL;
		}
	} else {
		// txbuf allocation failed
		rxbuf = NULL;
	}
	Py_END_ALLOW_THREADS
	if (!txbuf || !rxbuf) {
		// Allocation failed. Buffers has been freed already
		Py_DECREF(seq);
		Py_DECREF(rx_tuple);
		PyErr_SetString(PyExc_OverflowError, wrmsg_oom);
		return NULL;
	}


	block_start = 0;
	while (block_start < len) {

		for (ii = 0, jj = block_start; jj < len && ii < bufsize; ii++, jj++) {
			PyObject *val = PySequence_Fast_GET_ITEM(seq, jj);
#if PY_MAJOR_VERSION < 3
			if (PyInt_Check(val)) {
				txbuf[ii] = (__u8)PyInt_AS_LONG(val);
			} else
#endif
			{
				if (PyLong_Check(val)) {
					txbuf[ii] = (__u8)PyLong_AS_LONG(val);
				} else {
					snprintf(wrmsg_text, sizeof (wrmsg_text) - 1, wrmsg_val, val);
					PyErr_SetString(PyExc_TypeError, wrmsg_text);
					free(txbuf);
					free(rxbuf);
					Py_DECREF(rx_tuple);
					Py_DECREF(seq);
					return NULL;
				}
			}
		}

		block_size = ii;

		Py_BEGIN_ALLOW_THREADS
		xfer.tx_buf = (unsigned long)txbuf;
		xfer.rx_buf = (unsigned long)rxbuf;
		xfer.len = block_size;
		xfer.delay_usecs = delay_usecs;
		xfer.speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
		xfer.bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;

		status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
		Py_END_ALLOW_THREADS

		if (status < 0) {
			PyErr_SetFromErrno(PyExc_IOError);
			free(txbuf);
			free(rxbuf);
			Py_DECREF(rx_tuple);
			Py_DECREF(seq);
			return NULL;
		}
		for (ii = 0, jj = block_start; ii < block_size; ii++, jj++) {
			PyObject *val = PyLong_FromLong((long)rxbuf[ii]);
			PyTuple_SetItem(rx_tuple, jj, val);  // Steals reference, no need to Py_DECREF(val)
		}

		block_start += block_size;
	}


	// WA:
	// in CS_HIGH mode CS isnt pulled to low after transfer
	// reading 0 bytes doesn't really matter but brings CS down
	// tomdean:
	// Stop generating an extra CS except in mode CS_HIGH
	if (self->read0 && (self->mode & SPI_CS_HIGH)) status = read(self->fd, &rxbuf[0], 0);

	Py_BEGIN_ALLOW_THREADS
	free(txbuf);
	free(rxbuf);
	Py_END_ALLOW_THREADS

	Py_DECREF(seq);

	return rx_tuple;
}

PyDoc_STRVAR(MAX3100_fileno_doc,
	"fileno() -> integer \"file descriptor\"\n\n"
	"This is needed for lower-level file interfaces, such as os.read().\n");

static PyObject *
MAX3100_fileno(MAX3100_Object *self)
{
	PyObject *result = Py_BuildValue("i", self->fd);
	Py_INCREF(result);
	return result;
}

PyDoc_STRVAR(MAX3100_open_doc,
	"open(bus, device, crystal, baud)\n\n"
	"Connects the object to the specified SPI device.\n"
	"open(X,Y,...) will open /dev/spidev<X>.<Y>\n");

static PyObject *
MAX3100_open(MAX3100_Object *self, PyObject *args, PyObject *kwds)
{
	int bus, device, crystal, baud;
	char path[SPIDEV_MAXPATH];
	uint8_t tmp8;
	uint32_t tmp32;
	static char *kwlist[] = {"bus", "device", "crystal", "baud", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "iiii:open", kwlist, &bus, &device, &crystal, &baud))
		return NULL;
	if (snprintf(path, SPIDEV_MAXPATH, "/dev/spidev%d.%d", bus, device) >= SPIDEV_MAXPATH) {
		PyErr_SetString(PyExc_OverflowError,
			"Bus and/or device number is invalid.");
		return NULL;
	}
  if ((self->fd = open(path, O_RDWR, 0)) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	if (ioctl(self->fd, SPI_IOC_RD_MODE, &tmp8) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->mode = tmp8;
	if (ioctl(self->fd, SPI_IOC_RD_BITS_PER_WORD, &tmp8) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->bits_per_word = tmp8;
	if (ioctl(self->fd, SPI_IOC_RD_MAX_SPEED_HZ, &tmp32) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->max_speed_hz = tmp32;

  uint16_t conf;
  if (crystal == 2)
  {
    switch (baud)
    {
      case 230400 : conf = MAX3100_CONF_BAUD_X2_230400; break;
      case 115200 : conf = MAX3100_CONF_BAUD_X2_115200; break;
      case 57600  : conf = MAX3100_CONF_BAUD_X2_57600; break;
      case 38400  : conf = MAX3100_CONF_BAUD_X2_38400; break;
      case 19200  : conf = MAX3100_CONF_BAUD_X2_19200; break;
      case 4800   : conf = MAX3100_CONF_BAUD_X2_4800; break;
      case 2400   : conf = MAX3100_CONF_BAUD_X2_2400; break;
      case 1200   : conf = MAX3100_CONF_BAUD_X2_1200; break;
      case 600    : conf = MAX3100_CONF_BAUD_X2_600; break;
      default     : conf = MAX3100_CONF_BAUD_X2_9600; break;
    }
  }
  else
  {
    switch (baud)
    {
      case 115200 : conf = MAX3100_CONF_BAUD_X1_115200; break;
      case 57600  : conf = MAX3100_CONF_BAUD_X1_57600; break;
      case 38400  : conf = MAX3100_CONF_BAUD_X1_38400; break;
      case 19200  : conf = MAX3100_CONF_BAUD_X1_19200; break;
      case 4800   : conf = MAX3100_CONF_BAUD_X1_4800; break;
      case 2400   : conf = MAX3100_CONF_BAUD_X1_2400; break;
      case 1200   : conf = MAX3100_CONF_BAUD_X1_1200; break;
      case 600    : conf = MAX3100_CONF_BAUD_X1_600; break;
      case 300    : conf = MAX3100_CONF_BAUD_X1_300; break;
      default     : conf = MAX3100_CONF_BAUD_X1_9600; break;
    }
  }	

  // Do we want the MAX3100_CONF_RM? What does this mean for us?
  conf |= (MAX3100_CMD_WRITE_CONF | MAX3100_CONF_RM);
  write(self->fd, &conf, sizeof(uint16_t));
	// fprintf(stderr, "blah blah\n");

	Py_INCREF(Py_None);
	return Py_None;
}

void transfer16(int fd, uint16_t send, uint16_t *recv) {
	write(fd, &send, sizeof(uint16_t));
	read(fd, recv, sizeof(uint16_t));
}

static int
MAX3100_init(MAX3100_Object *self, PyObject *args, PyObject *kwds)
{
	int bus = -1;
	int client = -1;
	int crystal = -1;
	int baud = -1;
	static char *kwlist[] = {"bus", "client", "crystal", "baud", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiii:__init__",
			kwlist, &bus, &client, &crystal, &baud))
		return -1;

	if (bus >= 0) {
		MAX3100_open(self, args, kwds);
		if (PyErr_Occurred())
			return -1;
	}

	return 0;
}


PyDoc_STRVAR(MAX3100_ObjectType_doc,
	"MAX3100([bus],[client],[crystal],[baud]) -> Serial\n\n"
	"Return a new MAX3100 object that is (optionally) connected to the\n"
	"specified SPI device interface.\n");

static
PyObject *MAX3100_enter(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
        return NULL;

    Py_INCREF(self);
    return self;
}

static
PyObject *MAX3100_exit(MAX3100_Object *self, PyObject *args)
{

    PyObject *exc_type = 0;
    PyObject *exc_value = 0;
    PyObject *traceback = 0;
    if (!PyArg_UnpackTuple(args, "__exit__", 3, 3, &exc_type, &exc_value,
                           &traceback)) {
        return 0;
    }

    MAX3100_close(self);
    Py_RETURN_FALSE;
}

static PyMethodDef MAX3100_methods[] = {
	{"open", (PyCFunction)MAX3100_open, METH_VARARGS | METH_KEYWORDS,
		MAX3100_open_doc},
	{"close", (PyCFunction)MAX3100_close, METH_NOARGS,
		MAX3100_close_doc},
	{"fileno", (PyCFunction)MAX3100_fileno, METH_NOARGS,
		MAX3100_fileno_doc},
	{"readbytes", (PyCFunction)MAX3100_readbytes, METH_VARARGS,
		MAX3100_read_doc},
	{"writebytes", (PyCFunction)MAX3100_writebytes, METH_VARARGS,
		MAX3100_write_doc},
	{"writebytes2", (PyCFunction)MAX3100_writebytes2, METH_VARARGS,
		MAX3100_writebytes2_doc},
	{"xfer", (PyCFunction)MAX3100_xfer, METH_VARARGS,
		MAX3100_xfer_doc},
	{"xfer2", (PyCFunction)MAX3100_xfer2, METH_VARARGS,
		MAX3100_xfer2_doc},
	{"xfer3", (PyCFunction)MAX3100_xfer3, METH_VARARGS,
		MAX3100_xfer3_doc},
	{"__enter__", (PyCFunction)MAX3100_enter, METH_VARARGS,
		NULL},
	{"__exit__", (PyCFunction)MAX3100_exit, METH_VARARGS,
		NULL},
	{NULL},
};

static PyTypeObject MAX3100_ObjectType = {
#if PY_MAJOR_VERSION >= 3
	PyVarObject_HEAD_INIT(NULL, 0)
#else
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size */
#endif
	"MAX3100",			/* tp_name */
	sizeof(MAX3100_Object),		/* tp_basicsize */
	0,				/* tp_itemsize */
	(destructor)MAX3100_dealloc,	/* tp_dealloc */
	0,				/* tp_print */
	0,				/* tp_getattr */
	0,				/* tp_setattr */
	0,				/* tp_compare */
	0,				/* tp_repr */
	0,				/* tp_as_number */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping */
	0,				/* tp_hash */
	0,				/* tp_call */
	0,				/* tp_str */
	0,				/* tp_getattro */
	0,				/* tp_setattro */
	0,				/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
	MAX3100_ObjectType_doc,		/* tp_doc */
	0,				/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	0,				/* tp_iter */
	0,				/* tp_iternext */
	MAX3100_methods,			/* tp_methods */
	0,				/* tp_members */
	0,			  /* tp_getset */
	0,				/* tp_base */
	0,				/* tp_dict */
	0,				/* tp_descr_get */
	0,				/* tp_descr_set */
	0,				/* tp_dictoffset */
	(initproc)MAX3100_init,		/* tp_init */
	0,				/* tp_alloc */
	MAX3100_new,			/* tp_new */
};

static PyMethodDef MAX3100_module_methods[] = {
	{NULL}
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"max3100",
	MAX3100_module_doc,
	-1,
	MAX3100_module_methods,
	NULL,
	NULL,
	NULL,
	NULL,
};
#else
#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
#endif

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit_max3100(void)
#else
void initmax3100(void)
#endif
{
	PyObject* m;

	if (PyType_Ready(&MAX3100_ObjectType) < 0)
#if PY_MAJOR_VERSION >= 3
		return NULL;
#else
		return;
#endif

#if PY_MAJOR_VERSION >= 3
	m = PyModule_Create(&moduledef);
	PyObject *version = PyUnicode_FromString(_VERSION_);
#else
	m = Py_InitModule3("max3100", MAX3100_module_methods, MAX3100_module_doc);
	PyObject *version = PyString_FromString(_VERSION_);
#endif

	PyObject *dict = PyModule_GetDict(m);
	PyDict_SetItemString(dict, "__version__", version);
	Py_DECREF(version);

	Py_INCREF(&MAX3100_ObjectType);
	PyModule_AddObject(m, "MAX3100", (PyObject *)&MAX3100_ObjectType);

#if PY_MAJOR_VERSION >= 3
	return m;
#endif
}
