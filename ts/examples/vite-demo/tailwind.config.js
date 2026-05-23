/** @type {import('tailwindcss').Config} */
export default {
    // Class-based so the settings dialog can flip the theme on demand.
    // `useApplyTheme` in Settings.tsx toggles `dark` on <html>.
    darkMode: 'class',
    content: ['./index.html', './src/**/*.{ts,tsx}'],
    theme: { extend: {} },
    plugins: [],
};
