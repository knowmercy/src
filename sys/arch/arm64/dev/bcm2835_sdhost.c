#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/evcount.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

struct bcm_sdhost_softc {
	struct device		 sc_dev;
	//struct intrsource	 sc_bcm_intc_handler[INTC_NIRQ];
	//uint32_t		 sc_bcm_intc_imask[INTC_NBANK][NIPL];
	int32_t			 sc_localcoremask[MAXCPUS];
	bus_space_tag_t		 sc_iot;
	bus_space_handle_t	 sc_ioh;
	bus_space_handle_t	 sc_lioh;
	struct interrupt_controller sc_intc;
	struct interrupt_controller sc_l1_intc;
};
struct bcm_sdhost_softc *bcm_sdhost;


int bcm_sdhost_match(struct device *, void *, void *);

struct cfattach	bcmsdhost_ca = {
	sizeof (struct bcm_sdhost_softc), bcm_sdhost_match
};

struct cfdriver bcmsdhost_cd = {
	NULL, "bcmsdhost", DV_DULL
};



int
bcm_sdhost_match(struct device *parent, void *cfdata, void *aux)
{
	struct fdt_attach_args *faa = aux;

	if (OF_is_compatible(faa->fa_node, "brcm,bcm2835-sdhost"))
		return 1;
	return 0;
}
