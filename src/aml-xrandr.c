#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <string.h>

int
main (int argc, char **argv)
{
  Display *display;
  Window root;
  XRRScreenResources *resources;

  if (argc < 2 || strcmp (argv[1], "-q") != 0)
    return 1;

  fprintf (stderr, "AML xrandr helper invoked\n");

  display = XOpenDisplay (NULL);
  if (display == NULL)
    return 1;

  root = DefaultRootWindow (display);
  resources = XRRGetScreenResourcesCurrent (display, root);
  if (resources == NULL)
    {
      XCloseDisplay (display);
      return 1;
    }

  printf ("Screen 0: minimum 8 x 8, current %dx%d, maximum %dx%d\n",
          DisplayWidth (display, 0), DisplayHeight (display, 0),
          DisplayWidth (display, 0), DisplayHeight (display, 0));

  for (int i = 0; i < resources->noutput; i++)
    {
      XRROutputInfo *output = XRRGetOutputInfo (display, resources,
                                                resources->outputs[i]);
      XRRCrtcInfo *crtc = NULL;
      RRCrtc crtc_id = output != NULL ? output->crtc : None;

      if (output == NULL)
        continue;
      if (crtc_id != None)
        crtc = XRRGetCrtcInfo (display, resources, crtc_id);

      if (output->connection == RR_Connected)
        {
          if (crtc != NULL && crtc->width > 0 && crtc->height > 0)
            printf ("%s connected primary %dx%d+%d+%d (normal)\n",
                    output->name, crtc->width, crtc->height,
                    crtc->x, crtc->y);
          else
            printf ("%s connected\n", output->name);
        }
      else if (output->connection == RR_Disconnected)
        printf ("%s disconnected\n", output->name);

      for (int j = 0; j < output->nmode; j++)
        for (int k = 0; k < resources->nmode; k++)
          if (resources->modes[k].id == output->modes[j])
            {
              XRRModeInfo *mode = &resources->modes[k];
              double rate = 0.0;

              if (mode->hTotal != 0 && mode->vTotal != 0)
                rate = (double) mode->dotClock /
                       ((double) mode->hTotal * mode->vTotal);
              printf ("  %ux%u %.2f%s\n", mode->width, mode->height,
                      rate, crtc != NULL && crtc->mode == mode->id
                               ? "*+" : "");
            }

      if (crtc != NULL)
        XRRFreeCrtcInfo (crtc);
      XRRFreeOutputInfo (output);
    }

  XRRFreeScreenResources (resources);
  XCloseDisplay (display);
  return 0;
}
