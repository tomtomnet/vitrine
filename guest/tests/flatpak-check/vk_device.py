#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Vulkan devices a sandbox gets, by name, kind and API version, and a logical device made on
each that is no CPU (llvmpipe): the driver loads and answers. Pure ctypes, for runtimes without
vulkaninfo (vitrine's Mesa for Flatpak apps, 2026-11-15).
    python3 vk_device.py"""
import ctypes

vk = ctypes.CDLL('libvulkan.so.1')
vp, u32 = ctypes.c_void_p, ctypes.c_uint32


class InstanceCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', vp), ('flags', u32), ('pApplicationInfo', vp),
                ('enabledLayerCount', u32), ('ppEnabledLayerNames', vp),
                ('enabledExtensionCount', u32), ('ppEnabledExtensionNames', vp)]


class QueueCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', vp), ('flags', u32), ('queueFamilyIndex', u32),
                ('queueCount', u32), ('pQueuePriorities', ctypes.POINTER(ctypes.c_float))]


class DeviceCreateInfo(ctypes.Structure):
    _fields_ = [('sType', ctypes.c_int), ('pNext', vp), ('flags', u32), ('queueCreateInfoCount', u32),
                ('pQueueCreateInfos', ctypes.POINTER(QueueCreateInfo)), ('enabledLayerCount', u32),
                ('ppEnabledLayerNames', vp), ('enabledExtensionCount', u32),
                ('ppEnabledExtensionNames', vp), ('pEnabledFeatures', vp)]


vk.vkCreateInstance.argtypes = [ctypes.POINTER(InstanceCreateInfo), vp, ctypes.POINTER(vp)]
vk.vkEnumeratePhysicalDevices.argtypes = [vp, ctypes.POINTER(u32), ctypes.POINTER(vp)]
vk.vkGetPhysicalDeviceProperties.argtypes = [vp, ctypes.c_char_p]
vk.vkCreateDevice.argtypes = [vp, ctypes.POINTER(DeviceCreateInfo), vp, ctypes.POINTER(vp)]
vk.vkDestroyDevice.argtypes = [vp, vp]
vk.vkDestroyDevice.restype = None
inst = vp()
r = vk.vkCreateInstance(ctypes.byref(InstanceCreateInfo(sType=1)), None, ctypes.byref(inst))
if r != 0:
    raise SystemExit(f'vulkan: vkCreateInstance failed: {r}')
n = u32(0)
vk.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), None)
devs = (vp * n.value)()
vk.vkEnumeratePhysicalDevices(inst, ctypes.byref(n), devs)
kinds = {0: 'other', 1: 'integrated GPU', 2: 'discrete GPU', 3: 'virtual GPU', 4: 'CPU'}
seen = set()
for d in devs:
    props = ctypes.create_string_buffer(4096)   # VkPhysicalDeviceProperties, limits and all
    vk.vkGetPhysicalDeviceProperties(d, props)
    api = int.from_bytes(props.raw[0:4], 'little')
    kind = int.from_bytes(props.raw[16:20], 'little')
    name = props.raw[20:276].split(b'\0', 1)[0].decode()
    if name in seen:    # each ICD file lists it again
        continue
    seen.add(name)
    line = f'vulkan: {name} ({kinds.get(kind, kind)}), API {api >> 22 & 0x7f}.{api >> 12 & 0x3ff}.{api & 0xfff}'
    if kind != 4:
        prio = ctypes.c_float(1.0)
        queue = QueueCreateInfo(sType=2, queueFamilyIndex=0, queueCount=1, pQueuePriorities=ctypes.pointer(prio))
        info = DeviceCreateInfo(sType=3, queueCreateInfoCount=1, pQueueCreateInfos=ctypes.pointer(queue))
        dev = vp()
        r = vk.vkCreateDevice(d, ctypes.byref(info), None, ctypes.byref(dev))
        line += ': device made' if r == 0 else f': vkCreateDevice FAILED: {r}'
        if r == 0:
            vk.vkDestroyDevice(dev, None)
    print(line)
if not n.value:
    print('vulkan: no device')
