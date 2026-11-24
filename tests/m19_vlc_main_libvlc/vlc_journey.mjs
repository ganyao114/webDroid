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
