# Brook visual concepts

`data/wallpaper/brook-dawn.png` is the new landscape wallpaper, generated
with the built-in imagegen tool. `scripts/update_disk.sh` converts this
tracked source to ignored `data/wallpaper.raw` when needed. The converter
adds Brook's width/height header and XRGB pixels at 1920×1080.

The original attached wallpaper is preserved as
`data/wallpaper/brook-original-concept.png`. The original attached logo is
`docs/design/brook-original-logo.png`.

`data/branding/brook-mark.png` is the selected blue/teal B mark, shared by
the README, launcher and boot screen. It has real alpha in both the background
and river cutout, and its canvas is trimmed to the visible mark. The launcher
fits that canvas into its 24-pixel icon; the build bakes the same source against
black for boot. No ImageMagick dependency is needed to build from the tracked PNG.

The original opaque source is retained as `data/branding/brook-boot-mark.png`.
With user approval, the production cutout was derived with ImageMagick:

```sh
magick data/branding/brook-boot-mark.png -alpha off -separate \
  -evaluate-sequence max -threshold 12% \
  -define connected-components:area-threshold=500 \
  -define connected-components:mean-color=true -connected-components 8 \
  -blur 0x0.45 /tmp/brook-b-alpha.png
magick data/branding/brook-boot-mark.png /tmp/brook-b-alpha.png -alpha off \
  -compose CopyOpacity -composite -trim +repage data/branding/brook-mark.png
```

This removes the black matte and small isolated compression noise without
redesigning the B. The older swirl draft `data/branding/brook-mark-draft.png`
remains a design exploration; it is not used by the README, launcher or boot.
The retired rainbow PNGs and unused checked-in boot bitmap have been removed.
The prompt below records generation of the original opaque B source.

Built-in imagegen, generation mode, no input image, opaque background:

> Use case: logo-brand. Asset: Brook OS boot-screen logo on perfectly flat
> solid black background. Draw a clean, refined flat vector-style organic
> uppercase B symbol that suggests a winding brook. Two rounded asymmetrical
> lobes with a wide graceful flowing black negative-space river between them.
> Smooth saturated blue to teal gradient inside the solid silhouette, teal
> at top and blue below. Every boundary must be crisp and smooth; river and
> outer background must be pure black with absolutely no color fragments,
> stray particles, tiny speckles, or decorative strokes. Symbol only. No text,
> no white, no texture, no shadow, no glow, no bevel. Large centered symbol
> with generous black margins. Square composition. Designed for beautiful
> readable 200px boot logo.

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
