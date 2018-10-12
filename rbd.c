#include <stdio.h>
#include "lib/librados.h"
#include "lib/librbd.h"

rados_t ctx;
rados_ioctx_t ras;
rbd_image_t rbd;

int main()
{
    int ret;
    char buf[16];

    if ((ret = rados_create(&ctx, NULL)) < 0) {
        printf("rados create error\n");
        return ret;
    }
    if ((ret = rados_conf_read_file(ctx, "/etc/ceph/ceph.conf")) < 0) {
        printf("conf file error\n");
        goto out;
    }
    if ((ret = rados_connect(ctx)) < 0) {
        printf("rados connect error\n");
        goto out;
    }
    if ((ret = rados_ioctx_create(ctx, "tem", &ras)) < 0) {
        printf("rados create error\n");
        goto out;
    }
    if ((ret = rbd_open(ras, "mmm", &rbd, NULL)) < 0) {
        printf("rbd open error\n");
        rados_ioctx_destroy(ras);
        goto out;
    }
    if ((ret = rbd_read(rbd, 0, 6, buf)) != 6) {
        printf("rbd read error\n");
    }
    rbd_close(rbd);
    printf("success\n");

    out:
    rados_shutdown(ctx);
    return ret;
}
