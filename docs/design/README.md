# Brook visual concepts

`data/wallpaper/brook-dawn.png` is the new landscape wallpaper, generated
with the built-in imagegen tool. `scripts/update_disk.sh` converts this
tracked source to ignored `data/wallpaper.raw` when needed. The converter
adds Brook's width/height header and XRGB pixels at 1920×1080.

The original attached wallpaper is preserved as
`data/wallpaper/brook-original-concept.png`. The original attached logo is
`docs/design/brook-original-logo.png`.

`data/branding/brook-mark-draft.png` is a transparent blue/teal logo draft
using the original logo as a reference. It simplifies the river into one
broad cutout and removes the wordmark. This is a design exploration; edge
and transparency cleanup and small-size vector work remain before it
becomes the production logo. It is not used by the bootloader or desktop.

## Wallpaper prompt

Built-in imagegen, generation mode, no input image:

> Use case: stylized-concept. Asset type: a polished 16:9 desktop wallpaper
> for Brook OS, landscape 1920x1080 or larger. Create a tranquil flowing
> brook viewed from slightly above, curving through an open, misty landscape
> toward distant low hills. Water is deep blue and teal, reflecting a soft
> pale dawn sky; smooth stone banks and restrained grass silhouettes.
> Contemporary painterly realism, subtle atmospheric depth, elegant natural
> composition, gentle light. Quiet low-detail areas and soft gradients allow
> desktop windows and icons to remain readable. The water's flowing curve
> is the main visual gesture. No logos, no text, no UI, no borders, no people,
> no buildings, no watermark. This is a complete full-bleed wallpaper, not a
> computer or screen mockup.

## Logo prompts

Built-in imagegen, original attached logo as the visual reference,
transparent background:

> Use case: logo-brand. Asset type: a revised standalone Brook OS app icon /
> logo mark on a genuinely transparent background. Reference image 1 is the
> previous concept, used as a visual reference, not a final asset to preserve
> pixel-for-pixel. Retain its core idea: a soft, organic capital B silhouette
> containing a winding stream, in cool blue with a restrained teal upper
> area. Refine the concept for small-size readability: one broad, confident
> stream cutout rather than multiple hairline strands, two unmistakable
> rounded B lobes, balanced optical weight, a clean outer contour, strong
> silhouette at 32 px. Sophisticated flat design with at most a very subtle
> blue-to-teal gradient. Symbol only: remove the Brook wordmark and all text.
> Center the mark in a square canvas with generous even padding. No white
> rectangle, no shadow, no mockup, no bevels, no extra decorative lines. The
> river cutout and background must be transparent.

The generated mark was then used as the edit target with this prompt:

> Precisely clean this Brook logo mark. Preserve its silhouette, broad
> winding river cutout, centered composition, two B lobes, and blue-to-teal
> color palette. Change only the rendering quality: remove every stray dot,
> speckle, thin residual line, and fragment within the transparent river
> channel and outside the contour. Produce solid, smooth, clean-edged shapes
> with a very restrained smooth gradient. The entire river cutout is
> completely empty transparent negative space with no pixels inside it.
> The outside background is fully transparent. Crisp flat logo quality, no
> texture, no shadows, no text, no white or black background, no mockup.
