import { Component, ChangeDetectionStrategy, effect, inject, signal } from '@angular/core';
import { MatIconModule } from '@angular/material/icon';
import { MatButtonModule } from '@angular/material/button';
import { SystemDiagnosticService } from '../../services/system-diagnostic.service';
import { vendor, customHeadsetDriverName } from '../../../environment';

/**
 * Driver enable / lockout banner. Shown on every page that a user can land
 * on, so that someone migrating from the stock CustomHeadsetOpenVR install
 * sees the switch prompt regardless of which tab opens first.
 * Was inlined in driver-settings, which on vendor builds is no longer the
 * default tab.
 */
@Component({
    selector: 'app-driver-enable-banner',
    imports: [MatIconModule, MatButtonModule],
    templateUrl: './driver-enable-banner.component.html',
    styleUrl: './driver-enable-banner.component.scss',
    changeDetection: ChangeDetectionStrategy.OnPush
})
export class DriverEnableBannerComponent {
    public sds = inject(SystemDiagnosticService)
    show = signal(false)
    driverBlocked = signal(false)
    neutralDriverEnabled = signal(false)
    isVendor = !!vendor

    constructor() {
        effect(() => {
            const steamVrConfig = this.sds.steamVrConfig();
            if (!steamVrConfig) return;
            const customEnabled = this.sds.getSteamVRDriverEnableState(steamVrConfig, customHeadsetDriverName);
            this.driverBlocked.set(this.sds.isDriverBlocked(steamVrConfig, customHeadsetDriverName));
            if (vendor) {
                const neutralEnabled = this.sds.getNeutralDriverEnabled(steamVrConfig);
                this.neutralDriverEnabled.set(neutralEnabled);
                this.show.set(!customEnabled || neutralEnabled);
            } else {
                this.show.set(!customEnabled);
            }
        });
    }

    async enableDriver() {
        if (vendor) {
            await this.sds.enableVendorDriverAndDisableNeutral();
        } else {
            await this.sds.enableSteamVRDriver(customHeadsetDriverName);
        }
    }

    async unblockAllDrivers() {
        await this.sds.unblockAllDrivers();
    }
}
