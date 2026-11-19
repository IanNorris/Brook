"""Window rectangles for screenshot crops, from one hw_/vm_windows snapshot.

The window snapshot and the frame are separate reads (not atomic): a window
that moves between them is cropped at its snapshot position.
"""

AREAS = ('frame', 'client', 'buffer')


def find_window(listing, reference):
    """Return the snapshot row for an exact boot-bound window reference."""
    if listing.get('generation') != reference['session_generation']:
        raise ValueError('Stale session generation')
    if listing.get('guest_boot_id') != reference['guest_boot_id']:
        raise ValueError('Stale guest boot')
    for row in listing.get('windows', []):
        if row.get('window_id') == reference['window_id'] and row.get('incarnation') == reference['incarnation']:
            return row
    raise ValueError('Window not found; it may have closed. Take a new hw_windows/vm_windows snapshot')


def window_rect(row, area):
    """Screen rectangle (x, y, width, height) of the frame or client area.

    frame: the outer window including kernel chrome (title bar and borders).
    client: the client area only. Chrome metrics are derived from the outer
    and client sizes (symmetric side borders, the remainder above).
    """
    if area not in ('frame', 'client'):
        raise ValueError('area must be frame or client')
    if row.get('minimized') or not row.get('visible', True):
        raise ValueError('Window is minimized or hidden; restore it before a screen crop (or use window_area=buffer)')
    x, y = row['x'], row['y']
    outer_w, outer_h = row.get('outer_width') or row['width'], row.get('outer_height') or row['height']
    if area == 'frame':
        return x, y, outer_w, outer_h
    border = max(0, (outer_w - row['width']) // 2)
    top = max(0, outer_h - row['height'] - border)
    return x + border, y + top, row['width'], row['height']
