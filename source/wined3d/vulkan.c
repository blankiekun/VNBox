/* Host wined3d (HOST_WINED3D=1): the Vulkan backend isn't built (browsers have no Vulkan), so the Vulkan adapter
 * never exists; wined3d only asks for it when the vulkan renderer is configured. */
#include <stdarg.h>
#define COBJMACROS
#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

struct wined3d_adapter *wined3d_adapter_vk_create(unsigned int ordinal, unsigned int wined3d_creation_flags)
{
    ERR("No Vulkan on the host.\n");
    return NULL;
}
