#include <string.h>

#include "check.h"
#include "mbox/models.h"

int main(void)
{
    const mbox_model *m = mbox_model_by_usb(0x0dba, 0x3000);
    CHECK(m && strcmp(m->id, "mbox2") == 0);
    CHECK(m->support == MBOX_SUPPORT_SUPPORTED);
    CHECK(mbox_model_supports_rate(m, 48000) && !mbox_model_supports_rate(m, 96000));
    CHECK(m->playback.endpoint == 0x03 && m->capture.endpoint == 0x85);

    /* Models whose pid is unknown never match by USB id. */
    CHECK(mbox_model_by_usb(0x0dba, 0) == NULL);
    CHECK(mbox_model_by_id("mini") && mbox_model_by_id("mini")->pid == 0);
    CHECK(mbox_model_by_id("micro")->capture.endpoint == 0);
    CHECK(mbox_model_by_id("pro")->transport == MBOX_TRANSPORT_FIREWIRE);
    CHECK(mbox_model_by_id("nope") == NULL);

    /* Every USB profile is internally consistent. */
    for (size_t i = 0; i < mbox_model_count(); i++) {
        const mbox_model *x = mbox_model_at(i);
        CHECK(x->vid == MBOX_DIGIDESIGN_VID);
        if (x->transport != MBOX_TRANSPORT_USB)
            continue;
        CHECK(x->n_rates > 0);
        CHECK(x->playback.endpoint == 0 || !(x->playback.endpoint & 0x80));
        CHECK(x->capture.endpoint == 0 || (x->capture.endpoint & 0x80));
        CHECK((x->playback.endpoint != 0) == (x->playback.channels != 0));
        CHECK((x->capture.endpoint != 0) == (x->capture.channels != 0));
    }
    return CHECK_DONE();
}
