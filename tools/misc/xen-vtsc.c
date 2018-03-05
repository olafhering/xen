/* Could be merged into xen-diag.c? */

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xenctrl.h>

static uint32_t domid;
static uint32_t tsc_mode;
static uint64_t elapsed_nsec;
static uint32_t gtsc_khz;
static uint16_t vtsc_khz_tolerance;
static uint32_t incarnation;
static uint16_t new_vtsc_khz_tolerance;

static void show_help(void)
{
    fprintf(stderr, "Usage: xen-vtsc <domid> [vtsc_tolerance]\n");
}

int main(int argc, char *argv[])
{
    struct xc_interface_core *xch;
    int mode_set = 0;
    int rc;

    if (argc < 2 || argc > 3 || strcmp("-h", argv[1]) == 0)
    {
        show_help();
        return 0;
    }
    domid = atol(argv[1]);
    if (argc == 3)
    {
        unsigned long val;

        val = atol(argv[2]);
        if ( val > UINT16_MAX )
        {
            fprintf(stderr,
                    "Error: value for vtsc_tolerance must between 0 and %u\n", USHRT_MAX);
            return 1;
        }
        new_vtsc_khz_tolerance = val;
        if ( domid )
            mode_set = 1;
    }

    xch = xc_interface_open(0,0,0);
    if ( !xch )
    {
        fprintf(stderr, "failed to get xch handler\n");
        return 1;
    }



    if (mode_set)
    {
        rc = xc_domain_set_vtsc_khz_tolerance(xch, domid, new_vtsc_khz_tolerance);
        if ( rc )
        {
            perror("xc_domain_set_vtsc_khz_tolerance");
            goto err;
        }
    }
    else
    {
        rc = xc_domain_get_tsc_info(xch, domid, &tsc_mode, &elapsed_nsec, &gtsc_khz,
                                    &vtsc_khz_tolerance, &incarnation);
        if ( rc )
        {
            perror("xc_domain_get_tsc_info");
            goto err;
        }

        printf("domid: %" PRIu32 "\n"
               "tsc_mode: %" PRIu32 "\n"
               "elapsed_nsec: %" PRIu64 "\n"
               "gtsc_khz: %" PRIu32 "\n"
               "vtsc_khz_tolerance: %" PRIu16 "\n"
               "incarnation: %" PRIu32 "\n",
               domid, tsc_mode, elapsed_nsec, gtsc_khz, vtsc_khz_tolerance, incarnation);
    }

err:
    xc_interface_close(xch);

    return !!rc;
}
