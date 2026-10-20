    // HDLC CRC and Spinel framing have already been checked by OpenThread.
    // Commands use TID=0. Execution feedback is sent separately as a vendor
    // property notification by the OpenThread task, with the applied sequence.
    if (aCommand == H2_LED_SPINEL_COMMAND) {
        const uint8_t *data = nullptr;
        uint16_t length = 0;
        if (aHeader != 0x80 || mDecoder.ReadData(data, length) != OT_ERROR_NONE ||
            !h2_leds_control(data, length)) {
            return PrepareLastStatusResponse(aHeader, SPINEL_STATUS_INVALID_ARGUMENT);
        }
        return OT_ERROR_NONE;
    }
