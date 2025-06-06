#
# Copyright(c) 2019 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause-Clear
#

from ctypes import (
    c_void_p,
    c_int,
    c_uint32,
    c_uint64,
    CFUNCTYPE,
    Structure,
    POINTER,
    byref,
    cast,
)
from enum import IntEnum

from ..ocf import OcfLib
from .data import Data
from .queue import Queue


class IoDir(IntEnum):
    READ = 0
    WRITE = 1


class IoOps(Structure):
    pass


class Io(Structure):
    START = CFUNCTYPE(None, c_void_p)
    HANDLE = CFUNCTYPE(None, c_void_p, c_void_p)
    END = CFUNCTYPE(None, c_void_p, c_int)

    _instances_ = {}
    _fields_ = [
        ("_volume", c_void_p),
        ("_ops", POINTER(IoOps)),
        ("_addr", c_uint64),
        ("_flags", c_uint64),
        ("_bytes", c_uint32),
        ("_class", c_uint32),
        ("_dir", c_uint32),
        ("_io_queue", c_void_p),
        ("_start", START),
        ("_handle", HANDLE),
        ("_end", END),
        ("_priv1", c_void_p),
        ("_priv2", c_void_p),
    ]

    @classmethod
    def from_pointer(cls, ref):
        c = cls.from_address(ref)
        cls._instances_[ref] = c
        OcfLib.getInstance().ocf_io_set_cmpl_wrapper(
            byref(c), None, None, c.c_end
        )
        return c

    @classmethod
    def get_instance(cls, ref):
        return cls._instances_[cast(ref, c_void_p).value]

    def del_object(self):
        del type(self)._instances_[cast(byref(self), c_void_p).value]

    def put(self):
        OcfLib.getInstance().ocf_io_put(byref(self))

    def get(self):
        OcfLib.getInstance().ocf_io_get(byref(self))

    @staticmethod
    @END
    def c_end(io, err):
        Io.get_instance(io).end(err)

    @staticmethod
    @START
    def c_start(io):
        Io.get_instance(io).start()

    @staticmethod
    @HANDLE
    def c_handle(io, opaque):
        Io.get_instance(io).handle(opaque)

    def end(self, err):
        try:
            self.callback(err)
        except:  # noqa E722
            pass

        self.put()
        self.del_object()

    def submit(self):
        return OcfLib.getInstance().ocf_core_submit_io_wrapper(byref(self))

    def configure(
        self, addr: int, length: int, direction: IoDir, io_class: int, flags: int
    ):
        OcfLib.getInstance().ocf_io_configure_wrapper(
            byref(self), addr, length, direction, io_class, flags
        )

    def set_data(self, data: Data, offset: int = 0):
        self.data = data
        OcfLib.getInstance().ocf_io_set_data_wrapper(byref(self), data, offset)

    def set_queue(self, queue: Queue):
        OcfLib.getInstance().ocf_io_set_queue_wrapper(byref(self), queue.handle)


IoOps.SET_DATA = CFUNCTYPE(c_int, POINTER(Io), c_void_p, c_uint32)
IoOps.GET_DATA = CFUNCTYPE(c_void_p, POINTER(Io))

IoOps._fields_ = [("_set_data", IoOps.SET_DATA), ("_get_data", IoOps.GET_DATA)]

lib = OcfLib.getInstance()
lib.ocf_core_new_io_wrapper.restype = POINTER(Io)
lib.ocf_io_set_cmpl_wrapper.argtypes = [POINTER(Io), c_void_p, c_void_p, Io.END]
lib.ocf_io_configure_wrapper.argtypes = [
    POINTER(Io),
    c_uint64,
    c_uint32,
    c_uint32,
    c_uint32,
    c_uint64,
]
lib.ocf_io_set_queue_wrapper.argtypes = [POINTER(Io), c_uint32]

lib.ocf_core_new_io_wrapper.argtypes = [c_void_p]
lib.ocf_core_new_io_wrapper.restype = c_void_p

lib.ocf_io_set_data_wrapper.argtypes = [POINTER(Io), c_void_p, c_uint32]
lib.ocf_io_set_data_wrapper.restype = c_int

lib.ocf_io_set_queue_wrapper.argtypes = [POINTER(Io), c_void_p]
