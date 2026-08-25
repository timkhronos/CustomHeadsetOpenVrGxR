import { Routes } from '@angular/router';

export const routes: Routes = [
    { path: 'driver-settings', loadComponent: () => import('./pages/driver-settings/driver-settings.component').then(x => x.DriverSettingsComponent) },
    { path: 'distortion-profile', loadComponent: () => import('./pages/distortion-profile/distortion-profile.component').then(x => x.DistortionProfileComponent) },
    { path: 'stream-frame', loadComponent: () => import('./pages/stream-frame/stream-frame.component').then(x => x.StreamFrameComponent) },
    { path: 'app-settings', loadComponent: () => import('./pages/app-settings/app-settings.component').then(x => x.AppSettingsComponent) },
    { path: 'about', loadComponent: () => import('./pages/about/about.component').then(x => x.AboutComponent) },
    { path: '**', redirectTo: 'stream-frame' }
];
