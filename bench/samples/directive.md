Inline directives:

:cite[Smith *et al.*]{#smith .paper data-kind=ref} :badge[stable]{.green}
:cite[Smith *et al.*]{#smith .paper data-kind=ref} :badge[stable]{.green}
:cite[Smith *et al.*]{#smith .paper data-kind=ref} :badge[stable]{.green}
:cite[Smith *et al.*]{#smith .paper data-kind=ref} :badge[stable]{.green}
:cite[Smith *et al.*]{#smith .paper data-kind=ref} :badge[stable]{.green}

Leaf directives:

::youtube[Video of a *cat*]{vid=01ab2cd3efg .embed class=featured}
::banner[Launch]{#launch .wide data-kind=hero}
::status[]{state=green}

Container directives:

::::spoiler{#outer .red class=green .blue}
He dies.

:::spoiler
She is born.
:::
::::

Unclosed container:

::::spoiler
These three are not enough to close.
:::
So this line is also part of the container.

Malformed inline directives that should stay linear:

:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[
:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
