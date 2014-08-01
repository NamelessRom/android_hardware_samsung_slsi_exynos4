/*
 * Copyright (C) 2010 ARM Limited. All rights reserved.
 *
 * Portions of this code have been modified from the original.
 * These modifications are:
 *    * includes
 *    * enums
 *    * gralloc_device_open()
 *    * gralloc_register_buffer()
 *    * gralloc_unregister_buffer()
 *    * gralloc_lock()
 *    * gralloc_unlock()
 *    * gralloc_module_methods
 *    * HAL_MODULE_INFO_SYM
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <pthread.h>

#include <stdlib.h>
#include <sys/mman.h>
#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <fcntl.h>

#include "gralloc_priv.h"
#include "alloc_device.h"
#include "framebuffer_device.h"

#include "ump.h"
#include "ump_ref_drv.h"
#include "s5p_fimc.h"
#include "exynos_mem.h"

static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sMapLock = PTHREAD_MUTEX_INITIALIZER;


static int gSdkVersion = 0;
static int s_ump_is_open = 0;

#define PFX_NODE_MEM   "/dev/exynos-mem"

/* we need this for now because pmem cannot mmap at an offset */
#define PMEM_HACK   1

int get_bpp(int format)
{
    int bpp=0;

    switch(format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
        bpp=4;
        break;

    case HAL_PIXEL_FORMAT_RGB_888:
        bpp=3;
        break;

    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_RGBA_5551:
    case HAL_PIXEL_FORMAT_RGBA_4444:
   	    bpp=2;
   	    break;

    default:
        bpp=0;
    }

    ALOGD("%s bpp=%d", __func__, bpp);
    return bpp;
}

#ifdef USE_PARTIAL_FLUSH
struct private_handle_rect *rect_list;
static pthread_mutex_t s_rect_lock = PTHREAD_MUTEX_INITIALIZER;

private_handle_rect *find_rect(int secure_id)
{
    private_handle_rect *psRect;

    ALOGD("%s secure_id=%d",__func__,secure_id);
    pthread_mutex_lock(&s_rect_lock);

    for (psRect = rect_list; psRect; psRect = psRect->next)
        if (psRect->handle == secure_id)
            break;

    pthread_mutex_unlock(&s_rect_lock);
    return psRect;
}

private_handle_rect *find_last_rect(int secure_id)
{
    private_handle_rect *psRect;
    private_handle_rect *psFRect = NULL;

    ALOGD("%s secure_id=%d",__func__,secure_id);

    pthread_mutex_lock(&s_rect_lock);

    if (rect_list == NULL) {
        rect_list = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        psFRect = rect_list;
    } else {
        for (psRect = rect_list; psRect; psRect = psRect->next) {
            if (psRect->handle == secure_id)
                break;
            psFRect = psRect;
        }
    }

    pthread_mutex_unlock(&s_rect_lock);
    return psFRect;
}

int release_rect(int secure_id)
{
    int rc = 0;
    private_handle_rect *psRect;
    private_handle_rect *psTRect;

    ALOGD("%s secure_id=%d",__func__,secure_id);

    pthread_mutex_lock(&s_rect_lock);

    for (psRect = rect_list; psRect; psRect = psRect->next) {
        if (psRect->next) {
            if (psRect->next->handle == secure_id) {
                if (psRect->next->next)
                    psTRect = psRect->next->next;
                else
                    psTRect = NULL;

                free(psRect->next);
                psRect->next = psTRect;
                rc = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&s_rect_lock);

    return rc;
}
#endif

static int gralloc_map(gralloc_module_t const* module,
        buffer_handle_t handle, void** vaddr)
{
    ALOGE("%s", __func__);

    private_handle_t* hnd = (private_handle_t*)handle;
    if (!(hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            size_t size = FIMC1_RESERVED_SIZE * 1024;
            void *mappedAddress = mmap(0, size,
                    PROT_READ|PROT_WRITE, MAP_SHARED, gMemfd, (hnd->paddr - hnd->offset));
            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                return -errno;
            }
            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            size_t size = hnd->size;
            hnd->ion_client = ion_client_create();
            void *mappedAddress = ion_map(hnd->fd, size, 0);

            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not ion_map %s fd(%d)", strerror(errno), hnd->fd);
                return -errno;
            }

            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        } else {
            size_t size = hnd->size;
#if PMEM_HACK
            size += hnd->offset;
#endif
            void *mappedAddress = mmap(0, size,
                    PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd, 0);
            if (mappedAddress == MAP_FAILED) {
                ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                return -errno;
            }
            hnd->base = intptr_t(mappedAddress) + hnd->offset;
        }
    }
    *vaddr = (void*)hnd->base;
    return 0;
}

static int gralloc_unmap(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    ALOGE("%s",__func__);

	private_handle_t* hnd = (private_handle_t*)handle;
    if (!(hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            void* base = (void*)(intptr_t(hnd->base) - hnd->offset);
            size_t size = FIMC1_RESERVED_SIZE * 1024;
            if (munmap(base, size) < 0)
                ALOGE("Could not unmap %s", strerror(errno));
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            void* base = (void*)hnd->base;
            size_t size = hnd->size;
            if (ion_unmap(base, size) < 0)
                ALOGE("Could not ion_unmap %s", strerror(errno));
            ion_client_destroy(hnd->ion_client);
        } else {
            void* base = (void*)hnd->base;
            size_t size = hnd->size;
#if PMEM_HACK
            base = (void*)(intptr_t(base) - hnd->offset);
            size += hnd->offset;
#endif
            if (munmap(base, size) < 0)
                ALOGE("Could not unmap %s", strerror(errno));
        }
    }
    hnd->base = 0;
    return 0;
}

static int gralloc_device_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    int status = -EINVAL;
    char property[PROPERTY_VALUE_MAX];

    ALOGD("%s name=%s", __func__, name);
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0))
        status = alloc_device_open(module, name, device);
    else if (!strcmp(name, GRALLOC_HARDWARE_FB0))
        status = framebuffer_device_open(module, name, device);

    property_get("ro.build.version.sdk",property,0);
    gSdkVersion = atoi(property);

    return status;
}

static int gralloc_register_buffer(gralloc_module_t const* module, buffer_handle_t handle)
{
    int err = 0;
    int retval = -EINVAL;
    void *vaddr;
    bool cacheable=false;
    int rc=0;

    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Registering invalid buffer, returning error");
        return -EINVAL;
    }

    /* if this handle was created in this process, then we keep it as is. */
    private_handle_t* hnd = (private_handle_t*)handle;

    /*if (handle)
        ALOGE("%s sizeof(private_handle_t)=%d sizeof(private_handle_rect)=%d", __func__, sizeof(private_handle_t), sizeof(private_handle_rect)); */

    ALOGE("%s flags=%x", __func__, hnd->flags);

#ifdef USE_PARTIAL_FLUSH
    if (hnd->flags & (private_handle_t::PRIV_FLAGS_USES_UMP | private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
        private_handle_rect *psRect;
        private_handle_rect *psFRect;
        psRect = (private_handle_rect *)calloc(1, sizeof(private_handle_rect));
        psRect->handle = (int)hnd->ump_id;
        psRect->stride = (int) (hnd->stride * get_bpp(hnd->format));
        psFRect = find_last_rect((int)hnd->ump_id);
        psFRect->next = psRect;
    }
#endif

    /* not in stock
    // wjj, when WFD, use GRALLOC_USAGE_HW_VIDEO_ENCODER, 
    // ANW pid is same as SF pid, but need to create ump handle because WFD's BQ is in different pid.
    // gSdkVersion <= 17 means JB4.2, BQ always created by SF.
    if ((gSdkVersion <= 17) && (hnd->pid == getpid()) && (0 == (hnd->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)))
    {
        ALOGE("Unable to register handle 0x%x coming from different process: %d", (unsigned int)hnd, hnd->pid );
        return 0;
    }

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
        err = gralloc_map(module, handle, &vaddr); */

    pthread_mutex_lock(&s_map_lock);

    if (!s_ump_is_open) {
        ump_result res = ump_open(); /* TODO: Fix a ump_close() somewhere??? */
        if (res != UMP_OK) {
            pthread_mutex_unlock(&s_map_lock);
            ALOGE("Failed to open UMP library");
            return -EINVAL;
        }
        s_ump_is_open = 1;
    }

    /* not in stock hnd->pid = getpid(); */

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {

        hnd->ump_mem_handle = (int)ump_handle_create_from_secure_id(hnd->ump_id);
        ALOGE("%s PRIV_FLAGS_USES_UMP hnd->ump_mem_handle=%d(%x)", __func__, hnd->ump_mem_handle, hnd->ump_mem_handle);
        if (hnd->ump_mem_handle > 0) {
            hnd->base = (int)ump_mapped_pointer_get((ump_handle)hnd->ump_mem_handle);
            if (0 != hnd->base) {
                hnd->writeOwner = 0;
                hnd->lockState = 0;

                pthread_mutex_unlock(&s_map_lock);
                return 0;
            } else {
                ALOGE("Failed to map UMP handle");
                ump_reference_release((ump_handle)hnd->ump_mem_handle);
                pthread_mutex_unlock(&s_map_lock);
                return -EINVAL;
            }
        } else {
            ALOGE("Failed to create UMP handle");
            hnd->base = NULL;
            pthread_mutex_unlock(&s_map_lock);
            return -EINVAL;
        }


    } else {
        if (hnd->flags & (private_handle_t::PRIV_FLAGS_FRAMEBUFFER | private_handle_t::PRIV_FLAGS_USES_UMP) == 0) {
            ALOGE("%s PRIV_FLAGS_USES_PMEM mapping base=%x", __func__, hnd->base);

            pthread_mutex_unlock(&s_map_lock);
            return 0;
        } else if (hnd->flags == (private_handle_t::PRIV_FLAGS_USES_IOCTL | private_handle_t::PRIV_FLAGS_USES_HDMI)) {
            ALOGE("%s registering non-UMP buffer not supported", __func__);

            pthread_mutex_unlock(&s_map_lock);
    	    return -EINVAL;
        } else {
            void* vaddr = NULL;

   	        if (gMemfd == 0) {
                gMemfd = open(PFX_NODE_MEM, O_RDWR);
                if (gMemfd < 0) {
                    ALOGE("%s:: %s exynos-mem open error\n", __func__, PFX_NODE_MEM);
    	            pthread_mutex_unlock(&s_map_lock);
    	            return 0;
    	        }
    	    }

   	        cacheable = true;
    	    if (hnd->flags & private_handle_t::PRIV_FLAGS_NONE_CACHED)
    	        cacheable = false;

    	    ALOGE("%s cacheable=%d", __func__, cacheable);

            rc = ioctl(gMemfd, EXYNOS_MEM_SET_CACHEABLE, &cacheable);
            if (rc < 0)
                ALOGE("%s: Unable to set EXYNOS_MEM_SET_CACHEABLE to %d", __func__, cacheable);

            if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
                pthread_mutex_unlock(&s_map_lock); //?
                return 0;
            } else {
                if (hnd->flags & (private_handle_t::PRIV_FLAGS_USES_IOCTL | private_handle_t::PRIV_FLAGS_USES_HDMI)) {
                    vaddr = mmap(0, hnd->yaddr * 1024, PROT_READ|PROT_WRITE, MAP_SHARED, gMemfd, (hnd->paddr - hnd->offset));

                    if (vaddr == MAP_FAILED) {
                        ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                        pthread_mutex_unlock(&s_map_lock);
                        return -errno;
                    }

                } else {
                	vaddr = mmap(0, hnd->size + hnd->offset, PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd, 0);

                	if (vaddr == MAP_FAILED) {
                	    ALOGE("Could not mmap %s fd(%d)", strerror(errno),hnd->fd);
                	    pthread_mutex_unlock(&s_map_lock);
                	    return -errno;
                    }

                }

                hnd->base = intptr_t(vaddr) + hnd->offset;
                pthread_mutex_unlock(&s_map_lock);
               	return 0;
            }
        }
    }
}

static int gralloc_unregister_buffer(gralloc_module_t const* module, buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("%s unregistering invalid buffer, returning error", __func__);
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

    ALOGE("%s hnd->flags=%d", __func__, hnd->flags);

#ifdef USE_PARTIAL_FLUSH
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP)
        if (!release_rect((int)hnd->ump_id))
            ALOGE("%s hnd->ump_id=%d, release error", __func__, (int)hnd->ump_id);
#endif
    ALOGE_IF(hnd->lockState & private_handle_t::LOCK_STATE_READ_MASK,
            "%s [unregister] handle %p still locked (state=%08x)", __func__, hnd, hnd->lockState);

    /* never unmap buffers that were not registered in this process */
    /* not in stock if (hnd->pid == getpid()) { */
        pthread_mutex_lock(&s_map_lock);

        ALOGD("%s pthread_mutex_lock(&s_map_lock) executed succesfully", __func__);
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
            ALOGD("%s private_handle_t::PRIV_FLAGS_USES_UMP   hnd->ump_mem_handle=%d", __func__, hnd->ump_mem_handle);
            ump_mapped_pointer_release((ump_handle)hnd->ump_mem_handle);
            ump_reference_release((ump_handle)hnd->ump_mem_handle);

            hnd->base = 0;
            hnd->ump_mem_handle = (int)UMP_INVALID_MEMORY_HANDLE;

            hnd->lockState  = 0;
            hnd->writeOwner = 0;
        } else if (hnd->flags & (private_handle_t::PRIV_FLAGS_USES_IOCTL | private_handle_t::PRIV_FLAGS_USES_HDMI)) {
            ALOGE("%s unmmap  pid=%d, vaddr=%x)",__func__, hnd->pid, hnd->base);

            if (hnd->base != 0) {
                if (!(hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)) {
                    if (hnd->flags & (private_handle_t::PRIV_FLAGS_USES_IOCTL | private_handle_t::PRIV_FLAGS_USES_HDMI)) { //this will always be true
                        ALOGE("%s unmmap from %x size %d", __func__, hnd->base - hnd->offset, hnd->yaddr * 1024);
                        if (munmap( (void *) (hnd->base - hnd->offset), hnd->yaddr * 1024) < 0)
                            ALOGE("Could not unmap %s", strerror(errno));
                    }
                }
            }
        } else /*if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            if(hnd->base != 0)
                gralloc_unmap(module, handle);

            pthread_mutex_unlock(&s_map_lock);
            if (0 < gMemfd) {
                close(gMemfd);
                gMemfd = 0;
            }
            return 0;
        } else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            ump_mapped_pointer_release((ump_handle)hnd->ump_mem_handle);
            ump_reference_release((ump_handle)hnd->ump_mem_handle);
            if (hnd->base)
                gralloc_unmap(module, handle);

            hnd->base = 0;
            hnd->ump_mem_handle = (int)UMP_INVALID_MEMORY_HANDLE;
            hnd->lockState  = 0;
            hnd->writeOwner = 0;
        } else */ {
            ALOGE("%s unregistering non-UMP buffer not supported", __func__);
        }

        hnd->base == NULL;
        pthread_mutex_unlock(&s_map_lock);
    /*} */

    return 0;
}

static int gralloc_lock(gralloc_module_t const* module, buffer_handle_t handle,
                        int usage, int l, int t, int w, int h, void** vaddr)
{
    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Locking invalid buffer, returning error");
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

    ALOGE("%s hnd->flags=%x usage=%d l=%d t=%d w=%d h=%d", __func__, hnd->flags, usage, l, t, w, h);

#ifdef SAMSUNG_EXYNOS_CACHE_UMP
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
            ALOGD("%s private_handle_t::PRIV_FLAGS_USES_UMP hnd->ump_id=%d ", __func__, hnd->ump_id);
#ifdef USE_PARTIAL_FLUSH
            private_handle_rect *psRect;
            psRect = find_rect((int)hnd->ump_id);
            if (psRect) {
                psRect->l = l;
                psRect->t = t;
                psRect->w = w;
                psRect->h= h;
                psRect->locked = 1;
            }
#endif

            hnd->writeOwner = usage & GRALLOC_USAGE_SW_WRITE_MASK;
        }
#endif

    if (usage & GRALLOC_USAGE_YUV_ADDR) {
        vaddr[0] = (void*)hnd->base;
        vaddr[1] = (void*)(hnd->base + hnd->uoffset);
        vaddr[2] = (void*)(hnd->base + hnd->uoffset + hnd->voffset);
        ALOGE("%s vaddr[0]=%x vaddr[1]=%x vaddr[2]=%x", __func__, vaddr[0], vaddr[1], vaddr[2]);
    } else {
        if ((usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK)) || (usage == 0))
            *vaddr = (void*)hnd->base;
        ALOGE("%s vaddr=%x hnd->base=%x", __func__, *vaddr, hnd->base);
    }
    return 0;
}

static int gralloc_unlock(gralloc_module_t const* module, buffer_handle_t handle)
{
    ump_cpu_msync_op ump_op = UMP_MSYNC_CLEAN;
    int ret;
    exynos_mem_flush_range mem;

    if (private_handle_t::validate(handle) < 0) {
        ALOGE("Unlocking invalid buffer, returning error");
        return -EINVAL;
    }

    private_handle_t* hnd = (private_handle_t*)handle;

    ALOGE("%s hnd->flags=%x", __func__, hnd->flags);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_UMP) {
        ALOGE("%s private_handle_t::PRIV_FLAGS_USES_UMP", __func__);

        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            ALOGE("%s private_handle_t::PRIV_FLAGS_USES_ION", __func__);

            if (hnd->flags & private_handle_t::PRIV_FLAGS_NONE_CACHED){
                ALOGE("%s private_handle_t::PRIV_FLAGS_NONE_CACHED exiting", __func__);
                return 0;
            }

            if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_HDMI) {
                ump_op = UMP_MSYNC_CLEAN_AND_INVALIDATE;
            }

            ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, ump_op, (void *)hnd->base, hnd->size);
        } else {
            ALOGD("%s hnd->ump_mem_handle=%d(%x)", __func__, hnd->ump_mem_handle, hnd->ump_mem_handle);
#ifdef SAMSUNG_EXYNOS_CACHE_UMP
#ifdef USE_PARTIAL_FLUSH
            private_handle_rect *psRect;
            psRect = find_rect((int)hnd->ump_id);
            if (psRect) {
                ALOGD("%s rect found hnd->base=%x (psRect->stride(%d) * psRect->t(%d))=%d psRect->stride * psRect->h(%d)=%d", __func__, hnd->base, psRect->stride, psRect->t, (psRect->stride * psRect->t), psRect->h, (psRect->stride * psRect->h));

                ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, UMP_MSYNC_CLEAN,
                        (void *)(hnd->base + (psRect->stride * psRect->t)), psRect->stride * psRect->h );
                return 0;
            } else {
                ump_cpu_msync_now((ump_handle)hnd->ump_mem_handle, UMP_MSYNC_CLEAN, NULL, 0);
            }
#endif
#endif
        }

    } else {
        if (hnd->flags & private_handle_t::PRIV_FLAGS_NONE_CACHED) {
            ALOGE("%s private_handle_t::PRIV_FLAGS_NONE_CACHED exiting", __func__);
            return 0;
        }

        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {
            ALOGE("%s private_handle_t::PRIV_FLAGS_USES_IOCTL mem.start=%x mem.length=%x", __func__, hnd->paddr, hnd->size);
            mem.start = hnd->paddr;
            mem.length = hnd->size;

            ret = ioctl(gMemfd, EXYNOS_MEM_PADDR_CACHE_CLEAN, &mem);
            if (ret < 0) {
                ALOGE("Error in exynos-mem : EXYNOS_MEM_PADDR_CACHE_CLEAN (%d)\n", ret);
                return 0;
            }
        } else {
            if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_HDMI) {
                ALOGE("%s private_handle_t::PRIV_FLAGS_USES_HDMI mem.start=%x mem.length=%x", __func__, hnd->paddr, hnd->size);
                mem.start = hnd->paddr;
                mem.length = hnd->size;

                ret = ioctl(gMemfd, EXYNOS_MEM_PADDR_CACHE_FLUSH, &mem);
                if (ret < 0) {
                    ALOGE("Error in exynos-mem : EXYNOS_MEM_PADDR_CACHE_FLUSH (%d)\n", ret);
                    return 0;
                }
            } else {
                ALOGE("%s returning 0", __func__);
                return 0;
            }
        }

        return 0;
    }


    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
        ion_msync(hnd->ion_client, hnd->fd, IMSYNC_DEV_TO_RW | IMSYNC_SYNC_FOR_DEV, hnd->size, hnd->offset);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_IOCTL) {


    }

    return 0;
}

static int gralloc_getphys(gralloc_module_t const* module, buffer_handle_t handle, void** paddr)
{
    ALOGE("%s",__func__);

    private_handle_t* hnd = (private_handle_t*)handle;
    paddr[0] = (void*)hnd->paddr;
    paddr[1] = (void*)(hnd->paddr + hnd->uoffset);
    paddr[2] = (void*)(hnd->paddr + hnd->uoffset + hnd->voffset);
    return 0;
}

/* There is one global instance of the module */
static struct hw_module_methods_t gralloc_module_methods =
{
    open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM =
{
    base:
    {
        common:
        {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: GRALLOC_HARDWARE_MODULE_ID,
            name: "Graphics Memory Allocator Module",
            author: "ARM Ltd.",
            methods: &gralloc_module_methods,
            dso: NULL,
            reserved : {0,},
        },
        registerBuffer: gralloc_register_buffer,
        unregisterBuffer: gralloc_unregister_buffer,
        lock: gralloc_lock,
        unlock: gralloc_unlock,
        getphys: gralloc_getphys,
        perform: NULL,
        lock_ycbcr: NULL,
        reserved_proc: {0,},
    },
    framebuffer: NULL,
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: NULL,
    ion_client: -1,
};
