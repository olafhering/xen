#include <xen/init.h>
#include <xen/param.h>
#include <asm/msr.h>

/*
 * Valid values:
 *   1 => Explicit tsx=1
 *   0 => Explicit tsx=0
 *  -1 => Default, altered to 0/1 (if unspecified) by:
 *                 - TAA heuristics/settings for speculative safety
 *                 - "TSX vs PCR3" select for TSX memory ordering safety
 *  -3 => Implicit tsx=1 (feed-through from spec-ctrl=0)
 *
 * This is arranged such that the bottom bit encodes whether TSX is actually
 * disabled, while identifying various explicit (>=0) and implicit (<0)
 * conditions.
 */
int8_t __read_mostly opt_tsx = -1;
int8_t __read_mostly cpu_has_tsx_ctrl = -1;
bool __read_mostly rtm_disabled;

static int __init parse_tsx(const char *s)
{
    int rc = 0, val = parse_bool(s, NULL);

    if ( val >= 0 )
        opt_tsx = val;
    else
        rc = -EINVAL;

    return rc;
}
custom_param("tsx", parse_tsx);

void tsx_init(void)
{
    /*
     * This function is first called between microcode being loaded, and CPUID
     * being scanned generally.  Read into boot_cpu_data.x86_capability[] for
     * the cpu_has_* bits we care about using here.
     */
    if ( unlikely(cpu_has_tsx_ctrl < 0) )
    {
        uint64_t caps = 0;

        if ( boot_cpu_data.cpuid_level >= 7 )
            boot_cpu_data.x86_capability[cpufeat_word(X86_FEATURE_ARCH_CAPS)]
                = cpuid_count_edx(7, 0);

        if ( cpu_has_arch_caps )
            rdmsrl(MSR_ARCH_CAPABILITIES, caps);

        cpu_has_tsx_ctrl = !!(caps & ARCH_CAPS_TSX_CTRL);

        if ( cpu_has_tsx_force_abort )
        {
            /*
             * On an early TSX-enable Skylake part subject to the memory
             * ordering erratum, with at least the March 2019 microcode.
             */

            /*
             * Probe for the June 2021 microcode which de-features TSX on
             * client parts.  (Note - this is a subset of parts impacted by
             * the memory ordering errata.)
             *
             * RTM_ALWAYS_ABORT enumerates the new functionality, but is also
             * read as zero if TSX_FORCE_ABORT.ENABLE_RTM has been set before
             * we run.
             *
             * Undo this behaviour in Xen's view of the world.
             */
            bool has_rtm_always_abort = cpu_has_rtm_always_abort;

            if ( !has_rtm_always_abort )
            {
                uint64_t val;

                rdmsrl(MSR_TSX_FORCE_ABORT, val);

                if ( val & TSX_ENABLE_RTM )
                    has_rtm_always_abort = true;
            }

            /*
             * Always force RTM_ALWAYS_ABORT, even if it currently visible.
             * If the user explicitly opts to enable TSX, we'll set
             * TSX_FORCE_ABORT.ENABLE_RTM and cause RTM_ALWAYS_ABORT to be
             * hidden from the general CPUID scan later.
             */
            if ( has_rtm_always_abort )
                setup_force_cpu_cap(X86_FEATURE_RTM_ALWAYS_ABORT);

            /*
             * If no explicit tsx= option is provided, pick a default.
             *
             * This deliberately overrides the implicit opt_tsx=-3 from
             * `spec-ctrl=0` because:
             * - parse_spec_ctrl() ran before any CPU details where know.
             * - We now know we're running on a CPU not affected by TAA (as
             *   TSX_FORCE_ABORT is enumerated).
             * - When RTM_ALWAYS_ABORT is enumerated, TSX malfunctions, so we
             *   only ever want it enabled by explicit user choice.
             *
             * Without RTM_ALWAYS_ABORT, leave TSX active.  In particular,
             * this includes SKX where TSX is still supported.
             *
             * With RTM_ALWAYS_ABORT, disable TSX.
             */
            if ( opt_tsx < 0 )
                opt_tsx = !cpu_has_rtm_always_abort;
        }

        /*
         * The TSX features (HLE/RTM) are handled specially.  They both
         * enumerate features but, on certain parts, have mechanisms to be
         * hidden without disrupting running software.
         *
         * At the moment, we're running in an unknown context (WRT hiding -
         * particularly if another fully fledged kernel ran before us) and
         * depending on user settings, may elect to continue hiding them from
         * native CPUID instructions.
         *
         * Xen doesn't use TSX itself, but use cpu_has_{hle,rtm} for various
         * system reasons, mostly errata detection, so the meaning is more
         * useful as "TSX infrastructure available", as opposed to "features
         * advertised and working".
         *
         * Force the features to be visible in Xen's view if we see any of the
         * infrastructure capable of hiding them.
         */
        if ( cpu_has_tsx_ctrl || cpu_has_tsx_force_abort )
        {
            setup_force_cpu_cap(X86_FEATURE_HLE);
            setup_force_cpu_cap(X86_FEATURE_RTM);
        }
    }

    /*
     * Note: MSR_TSX_CTRL is enumerated on TSX-enabled MDS_NO and later parts.
     * MSR_TSX_FORCE_ABORT is enumerated on TSX-enabled pre-MDS_NO Skylake
     * parts only.  The two features are on a disjoint set of CPUs, and not
     * offered to guests by hypervisors.
     */
    if ( cpu_has_tsx_ctrl )
    {
        uint32_t hi, lo;

        rdmsr(MSR_TSX_CTRL, lo, hi);

        /* Check bottom bit only.  Higher bits are various sentinels. */
        rtm_disabled = !(opt_tsx & 1);

        lo &= ~(TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR);
        if ( rtm_disabled )
            lo |= TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR;

        wrmsr(MSR_TSX_CTRL, lo, hi);
    }
    else if ( cpu_has_tsx_force_abort )
    {
        /*
         * On an early TSX-enable Skylake part subject to the memory ordering
         * erratum, with at least the March 2019 microcode.
         */
        uint32_t hi, lo;

        rdmsr(MSR_TSX_FORCE_ABORT, lo, hi);

        /* Check bottom bit only.  Higher bits are various sentinels. */
        rtm_disabled = !(opt_tsx & 1);

        lo &= ~(TSX_FORCE_ABORT_RTM | TSX_CPUID_CLEAR | TSX_ENABLE_RTM);

        if ( cpu_has_rtm_always_abort )
        {
            /*
             * June 2021 microcode, on a client part with TSX de-featured:
             *  - There are no mitigations for the TSX memory ordering errata.
             *  - Performance counter 3 works.  (I.e. it isn't being used by
             *    microcode to work around the memory ordering errata.)
             *  - TSX_FORCE_ABORT.FORCE_ABORT_RTM is fixed read1/write-discard.
             *  - TSX_FORCE_ABORT.TSX_CPUID_CLEAR can be used to hide the
             *    HLE/RTM CPUID bits.
             *  - TSX_FORCE_ABORT.ENABLE_RTM may be used to opt in to
             *    re-enabling RTM, at the users own risk.
             */
            lo |= rtm_disabled ? TSX_CPUID_CLEAR : TSX_ENABLE_RTM;
        }
        else
        {
            /*
             * Either a server part where TSX isn't de-featured, or pre-June
             * 2021 microcode:
             *  - By default, the TSX memory ordering errata is worked around
             *    in microcode at the cost of Performance Counter 3.
             *  - "Working TSX" vs "Working PCR3" can be selected by way of
             *    setting TSX_FORCE_ABORT.FORCE_ABORT_RTM.
             */
            if ( rtm_disabled )
                lo |= TSX_FORCE_ABORT_RTM;
        }

        wrmsr(MSR_TSX_FORCE_ABORT, lo, hi);
    }
    else if ( opt_tsx >= 0 )
        printk_once(XENLOG_WARNING
                    "TSX controls not available - Ignoring tsx= setting\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
