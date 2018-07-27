#include "PktSerialProtocol.h"
#include "CodalDmesg.h"
#include "Timer.h"

using namespace codal;

void PktLogicDriver::periodicCallback()
{
    // no sense continuing if we dont have a bus to transmit on...
    if (!proto.bus.isRunning())
        return;

    // for each driver we maintain a rolling counter, used to trigger various timer related events.
    // uint8_t might not be big enough in the future if the scheduler runs faster...
    for (int i = 0; i < PKT_PROTOCOL_DRIVER_SIZE; i++)
    {
        // ignore ourself
        if (proto.drivers[i] == NULL || proto.drivers[i] == this)
            continue;

        if (proto.drivers[i]->device.flags & (PKT_DEVICE_FLAGS_INITIALISED | PKT_DEVICE_FLAGS_INITIALISING))
            proto.drivers[i]->device.rolling_counter++;

        // if the driver is acting as a virtual driver, we don't need to perform any initialisation, just connect / disconnect events.
        if (proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_REMOTE)
        {
            if (proto.drivers[i]->device.rolling_counter == PKT_LOGIC_DRIVER_TIMEOUT)
            {
                if (!(proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_CP_SEEN))
                    proto.drivers[i]->deviceRemoved();

                proto.drivers[i]->device.flags &= ~(PKT_DEVICE_FLAGS_CP_SEEN);
                continue;
            }
        }

        // local drivers run on the device
        if (proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_LOCAL)
        {
            if (!(proto.drivers[i]->device.flags & (PKT_DEVICE_FLAGS_INITIALISED | PKT_DEVICE_FLAGS_INITIALISING)))
            {
                PKT_DMESG("BEGIN INIT");
                proto.drivers[i]->device.address = 0;

                bool allocated = true;

                // compute a reasonable first address
                while(allocated)
                {
                    bool stillAllocated = false;
                    proto.drivers[i]->device.address = target_random(256);

                    for (int j = 0; j < PKT_PROTOCOL_DRIVER_SIZE; j++)
                    {
                        if (i == j)
                            continue;

                        if (proto.drivers[j] && proto.drivers[j]->device.flags & PKT_DEVICE_FLAGS_INITIALISED)
                        {
                            if (proto.drivers[j]->device.address == proto.drivers[i]->device.address)
                            {
                                stillAllocated = true;
                                break;
                            }
                        }
                    }

                    allocated = stillAllocated;
                }

                PKT_DMESG("ALLOC: %d",proto.drivers[i]->device.address);

                proto.drivers[i]->queueControlPacket();
                proto.drivers[i]->device.flags |= PKT_DEVICE_FLAGS_INITIALISING;

            }
            else if(proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_INITIALISING)
            {
                // if no one has complained in a second, consider our address allocated
                if (proto.drivers[i]->device.rolling_counter == PKT_LOGIC_ADDRESS_ALLOC_TIME)
                {
                    PKT_DMESG("FINISHED");
                    proto.drivers[i]->device.flags &= ~PKT_DEVICE_FLAGS_INITIALISING;
                    proto.drivers[i]->device.flags |= PKT_DEVICE_FLAGS_INITIALISED;
                    proto.drivers[i]->deviceConnected(proto.drivers[i]->device);
                }
            }
            else if (proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_INITIALISED)
            {
                if(proto.drivers[i]->device.rolling_counter > 0 && (proto.drivers[i]->device.rolling_counter % PKT_LOGIC_DRIVER_CTRLPACKET_TIME) == 0)
                    proto.drivers[i]->queueControlPacket();
            }
        }
    }
}


PktLogicDriver::PktLogicDriver(PktSerialProtocol& proto, PktDevice d, uint32_t driver_class, uint16_t id) : PktSerialDriver(proto, d, driver_class, id)
{
    this->device.address = 0;
    status = 0;
    memset(this->address_filters, 0, PKT_LOGIC_DRIVER_MAX_FILTERS);

    // flags this instance as occupied
    this->device.flags = (PKT_DEVICE_FLAGS_LOCAL | PKT_DEVICE_FLAGS_INITIALISED);
}

void PktLogicDriver::handleControlPacket(ControlPacket* p)
{
    // nop for now... could be useful in the future
}

/**
  * Given a control packet, finds the associated driver, or if no associated device, associates a remote device with a driver.
  **/
void PktLogicDriver::handlePacket(PktSerialPkt* p)
{
    ControlPacket *cp = (ControlPacket *)p->data;

    PKT_DMESG("CP REC: %d, %d, %d", cp->address, cp->serial_number, cp->driver_class);

    // first check for any drivers who are associated with this control packet
    for (int i = 0; i < PKT_PROTOCOL_DRIVER_SIZE; i++)
    {
        if (proto.drivers[i] && proto.drivers[i]->device.address == cp->address)
        {
            PKT_DMESG("FINDING");
            // if we have allocated that address to one of our devices, respond with a conflict packet
            if (proto.drivers[i]->device.serial_number != cp->serial_number && !(proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_INITIALISING))
            {
                cp->flags |= CONTROL_PKT_FLAGS_CONFLICT;
                proto.bus.send((uint8_t*)cp, sizeof(ControlPacket), 0);
                return;
            }
            // someone has flagged a conflict with an initialising device
            if (proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_INITIALISING && cp->flags & CONTROL_PKT_FLAGS_CONFLICT)
            {
                // new address will be assigned on next tick.
                proto.drivers[i]->device.flags &= ~PKT_DEVICE_FLAGS_INITIALISING;
                return;
            }

            // flag as seen so we do not inadvertently disconnect a device.
            proto.drivers[i]->device.flags |= PKT_DEVICE_FLAGS_CP_SEEN;

            // for some drivers, pairing is required... pass the packet through to the driver.
            proto.drivers[i]->handleControlPacket(cp);
            return;
        }
    }

    bool filtered = filterPacket(cp->address);

    // if it's paired with another device, we can just ignore
    if (cp->flags & CONTROL_PKT_FLAGS_PAIRED && !filtered)
    {
        PKT_DMESG("FILTERING");
        for (int i = 0; i < PKT_LOGIC_DRIVER_MAX_FILTERS; i++)
        {
            if (this->address_filters[i] == 0)
                this->address_filters[i] = cp->address;
        }

        return;
    }

    // if it was previously paired with another device, we remove the filter.
    if (filtered && cp->flags & CONTROL_PKT_FLAGS_BROADCAST)
    {
        PKT_DMESG("UNDO FILTER");
        for (int i = 0; i < PKT_LOGIC_DRIVER_MAX_FILTERS; i++)
        {
            if (this->address_filters[i] == cp->address)
                this->address_filters[i] = 0;
        }

        // drop through...
    }

    // if we reach here, there is no associated device, find a free instance in the drivers array
    for (int i = 0; i < PKT_PROTOCOL_DRIVER_SIZE; i++)
    {
        PKT_DMESG("FIND DRIVER");
        if (proto.drivers[i] && proto.drivers[i]->device.flags & PKT_DEVICE_FLAGS_REMOTE && proto.drivers[i]->driver_class == cp->driver_class)
        {
            // this driver instance is looking for a specific serial number
            if (proto.drivers[i]->device.serial_number > 0 && proto.drivers[i]->device.serial_number != cp->serial_number)
                continue;

            PKT_DMESG("FOUND");
            PktDevice d;
            d.address = cp->address;
            d.rolling_counter = 0;
            d.flags = cp->flags;
            d.serial_number = cp->serial_number;

            proto.drivers[i]->deviceConnected(d);
            return;
        }

    }

    // if we reach here we just drop the packet.
}

bool PktLogicDriver::filterPacket(uint8_t address)
{
    if (address > 0)
    {
        for (int i = 0; i < PKT_PROTOCOL_DRIVER_SIZE; i++)
            if (address_filters[i] == address)
                return true;
    }

    return false;
}

void PktLogicDriver::start()
{
    status |= (DEVICE_COMPONENT_RUNNING | DEVICE_COMPONENT_STATUS_SYSTEM_TICK);
}

void PktLogicDriver::stop()
{
    status &= ~(DEVICE_COMPONENT_RUNNING | DEVICE_COMPONENT_STATUS_SYSTEM_TICK);
}