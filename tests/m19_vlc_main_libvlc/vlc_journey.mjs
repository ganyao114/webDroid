export const onboardingActivity = 'org.videolan.vlc.gui.onboarding.OnboardingActivity';
export const mainActivity = 'org.videolan.vlc.gui.MainActivity';
export const tabs = ['Video', 'Audio', 'Browse', 'Playlists', 'More'];
export const onboardingPages = ['Welcome to VLC!', 'Permission', 'Permission not granted', 'Make yourself at home'];

export function onboardingAction(labels) {
  for (const choice of ['done', 'next', 'continue', 'get started']) {
    const button = labels.find(({ text }) => text.trim().toLowerCase() === choice);
    if (button) return button;
  }
  return null;
}

export function validVlcPcm({ sink, frames, nonzeroFrames, peak, rms, frequency }) {
  return sink === 'OpenSLES' && frames >= 8640 && frames <= 12000 && nonzeroFrames > 8500 &&
    peak > 0.17 && peak < 0.26 && rms > 0.11 && rms < 0.19 && Math.abs(frequency - 440) < 5;
}
