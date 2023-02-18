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
#include <sys/time.h>

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
#define MAX3100_CONF_RM             0b0000110000000000

// Crystal
#define MAX3100_CRYSTAL_1843kHz     1
#define MAX3100_CRYSTAL_3686kHz     2

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

#define BYTE_TO_BINARY_PATTERN "%s %c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c\n"
#define BYTE_TO_BINARY(byte)  \
  (byte & 0x8000 ? '1' : '0'), \
  (byte & 0x4000 ? '1' : '0'), \
  (byte & 0x2000 ? '1' : '0'), \
  (byte & 0x1000 ? '1' : '0'), \
  (byte & 0x0800 ? '1' : '0'), \
  (byte & 0x0400 ? '1' : '0'), \
  (byte & 0x0200 ? '1' : '0'), \
  (byte & 0x0100 ? '1' : '0'), \
	(byte & 0x0080 ? '1' : '0'), \
  (byte & 0x0040 ? '1' : '0'), \
  (byte & 0x0020 ? '1' : '0'), \
  (byte & 0x0010 ? '1' : '0'), \
  (byte & 0x0008 ? '1' : '0'), \
  (byte & 0x0004 ? '1' : '0'), \
  (byte & 0x0002 ? '1' : '0'), \
  (byte & 0x0001 ? '1' : '0')
#define fprintf_binary(file, msg, value) fprintf(file, BYTE_TO_BINARY_PATTERN, msg, BYTE_TO_BINARY(value))

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
	uint8_t maxmisses;
} MAX3100_Object;

#define BUFSIZE 2048
uint8_t buffer[BUFSIZE];
/* good read chars go from bufst ... (bufend-1), buffer is empty if bufend == bufst */
uint32_t bufst=0;
uint32_t bufend=0;

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

uint16_t swapbytes(uint16_t data) {
	return ((data << 8) & 0xff00) | ((data >> 8) & 0x00ff);
}

void send16(int fd, uint16_t send) {
	// fprintf_binary(stderr, "send", send);
	send = swapbytes(send);
  write(fd, &send, sizeof(uint16_t));
}

uint16_t recv16(int fd) {
	uint16_t recv=0;
	read(fd, &recv, sizeof(uint16_t));
	recv = swapbytes(recv);
	// fprintf_binary(stderr, "recv", recv);
	return recv;
}
	
uint16_t transfer16(MAX3100_Object *self, uint16_t send) {
	uint16_t recv=0;
	// fprintf_binary(stderr, "send", send);
	send = swapbytes(send);
	struct spi_ioc_transfer xfer;
	memset(&xfer, 0, sizeof(xfer));
  xfer.tx_buf = (unsigned long)&send;
	xfer.rx_buf = (unsigned long)&recv;
	xfer.len = 2;
	xfer.delay_usecs = 0;
	xfer.speed_hz = self->max_speed_hz;
	xfer.bits_per_word = self->bits_per_word;
	int status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
	recv = swapbytes(recv);
	// fprintf_binary(stderr, "recv", recv);
	return recv;
}

void fetchbytes(MAX3100_Object *self) {
	uint16_t r;
	uint8_t misses = 0;
	while (misses < self->maxmisses) {
		r = transfer16(self, MAX3100_CMD_READ_DATA);
		if (r&MAX3100_CONF_R) {
		  buffer[bufend] = (uint8_t)(r&0xff);
		  bufend += 1
		  if (bufend >= BUFSIZE) {
		    bufend = 0;
		  }
			assert(bufend != bufst);
			misses = 0;
		} else {
			misses += 1;
		}
	}
}

void putbyte(MAX3100_Object *self, uint8_t uch)) {
	uint16_t r;
	while (1) {
		r = transfer16(self, MAX3100_CMD_READ_CONF);
		if (r&MAX3100_CONF_R) {
			fetchbytes(self);
		} else if (r&MAX3100_CONF_T) {
			break;
		}
	}
	r = transfer16(self,MAX3100_CMD_WRITE_DATA|uch);
	if (r&MAX3100_CONF_R) {
	  buffer[bufend] = (uint8_t)(r&0xff);
	  bufend += 1
		if (bufend >= BUFSIZE) {
		  bufend = 0;
		}
		assert(bufend != bufst);
		fetchbytes(self);
	}
}

uint8_t getbyte(MAX3100_Object *self, uint8_t &uch)) {
	if (bufend == bufst) {
		fetchbytes(self);
	}
	if (bufend != bufst) {
		*uch = buffer[bufst]
		bufst += 1
		if (bufst >= BUFSIZE) {
			bufst = 0;
		}
		return 1;
	} 
	return 0;
}

uint8_t readbyte(MAX3100_Object *self, uint8_t *ch) {
  uint16_t r = transfer16(self, MAX3100_CMD_READ_DATA);
  if (r&MAX3100_CONF_R) {
     (*ch) = (uint8_t)(r&0xff);
		 return (uint8_t)1;
	} else {
    return (uint8_t)0;
  }
}

uint16_t ready(MAX3100_Object *self) {
	uint16_t r = transfer16(self, MAX3100_CMD_READ_CONF);
  return (r&MAX3100_CONF_R);
}

uint16_t busy(MAX3100_Object *self) {
	uint16_t r = transfer16(self, MAX3100_CMD_READ_CONF);
	// fprintf_binary(stderr, "busy", r&MAX3100_CONF_T);
	return (!(r&MAX3100_CONF_T));
}

void writebyte(MAX3100_Object *self, uint8_t uch) {
	while (1) {
	  if (!busy(self)) {
			transfer16(self,MAX3100_CMD_WRITE_DATA|uch);
			break;
		}
	}
}

void then(struct timeval *tv, int usec) {
	struct timeval tv0, tv1;
	gettimeofday(&tv0,NULL);
	tv1.tv_sec = usec/1000000;
	tv1.tv_usec = usec%1000000;
	timeradd(&tv0,&tv1,tv);
}

int stopnow(struct timeval *stoptime) {
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return (timercmp(&tv,stoptime,>));
}

static char *wrmsg_list0 = "Empty argument list.";
static char *wrmsg_listmax = "Argument list size exceeds %d bytes.";
static char *wrmsg_val = "Non-Int/Long value in arguments: %x.";
// static char *wrmsg_oom = "Out of memory.";
static char *wrmsg_timeout = "Timeout.";

PyDoc_STRVAR(MAX3100_write_doc,
	"write([values]) -> None\n\n"
	"Write bytes to SPI device.\n");



static PyObject *
MAX3100_writebytes(MAX3100_Object *self, PyObject *args)
{
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

  // fprintf(stderr, "length: %d\n", len);
	for (ii = 0; ii < len; ii++) {
		PyObject *val = PySequence_Fast_GET_ITEM(seq, ii);
		// PyObject_Print((PyObject *)Py_TYPE(val), stderr, 0);
		// fprintf(stderr, "\n");
		// fprintf(stderr, "%d\n",PyLong_Check(val));
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

  for (int ii=0; ii<len; ii++) {
		putbyte(self, buf[ii]);
	}
	
	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(MAX3100_read_doc,
	"read(length=1) -> [values]\n\n"
	"Read len bytes from SPI device. timeout in seconds\n");

static PyObject *
MAX3100_readbytes(MAX3100_Object *self, PyObject *args, PyObject *kwds)
{
	uint8_t	rxbuf[SPIDEV_MAXPATH];
	int	ii;
	int len=1;
	
	PyObject	*list;
  static char *kwlist[] = {"length", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i:read", kwlist, &len))
		return NULL;
	
	/* read at least 1 byte, no more than SPIDEV_MAXPATH */
	if (len < 1)
		len = 1;
	else if ((unsigned)len > sizeof(rxbuf))
		len = sizeof(rxbuf);

	int ii = 0;
	memset(rxbuf, 0, sizeof rxbuf);
	while (ii < len) {
		if (getbyte(self, &(rxbuf[ii]))) {
	    ii++;
		}
	}
	
	list = PyList_New(len);

	for (ii = 0; ii < len; ii++) {
		PyObject *val = PyLong_FromLong((long)rxbuf[ii]);
		PyList_SET_ITEM(list, ii, val);  // Steals reference, no need to Py_DECREF(val)
	}

	return list;
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
	"open(bus=0, device=0, crystal=2, baud=9600, spispeed=3900000, maxmisses=3)\n\n"
	"Connects the object to the specified SPI device.\n"
	"open(X,Y,...) will open /dev/spidev<X>.<Y>\n");

static PyObject *
MAX3100_open(MAX3100_Object *self, PyObject *args, PyObject *kwds)
{
	int bus=0;
	int device=0;
	int crystal=2;
	int baud=9600;
	int spispeed = 3900000;
	int maxmisses = 3;
	char path[SPIDEV_MAXPATH];
	uint8_t tmp8;
	//uint32_t tmp32;
	static char *kwlist[] = {"bus", "device", "crystal", "baud", "spispeed", "maxmisses", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiiiii:open", kwlist, 
	                                 &bus, &device, &crystal, &baud, &spispeed, &maxmisses))
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
	
	uint32_t spispeed_ = spispeed;
	if (ioctl(self->fd, SPI_IOC_WR_MAX_SPEED_HZ, &spispeed_) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->max_speed_hz = spispeed_;
	self->maxmisses = maxmisses;

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
  transfer16(self, conf);
	
	Py_INCREF(Py_None);
	return Py_None;
}

static int
MAX3100_init(MAX3100_Object *self, PyObject *args, PyObject *kwds)
{
	int bus = -1;
	int client = -1;
	int crystal = -1;
	int baud = -1;
	int spispeed = -1;
	int maxmisses = -1;
	static char *kwlist[] = {"bus", "client", "crystal", "baud", "spispeed", "maxmisses", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiiiii:__init__",
			kwlist, &bus, &client, &crystal, &baud, &spispeed, &maxmisses))
		return -1;

	if (bus >= 0) {
		MAX3100_open(self, args, kwds);
		if (PyErr_Occurred())
			return -1;
	}

	return 0;
}


PyDoc_STRVAR(MAX3100_ObjectType_doc,
	"MAX3100([bus],[client],[crystal],[baud],[spispeed],[maxmisses]) -> Serial\n\n"
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
	{"read", (PyCFunction)MAX3100_readbytes, METH_VARARGS | METH_KEYWORDS,
		MAX3100_read_doc},
	{"write", (PyCFunction)MAX3100_writebytes, METH_VARARGS,
		MAX3100_write_doc},
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
