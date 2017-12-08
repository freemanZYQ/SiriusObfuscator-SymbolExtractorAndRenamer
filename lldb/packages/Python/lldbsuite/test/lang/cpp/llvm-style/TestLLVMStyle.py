from lldbsuite.test import lldbinline
from lldbsuite.test import decorators

lldbinline.MakeInlineTest(
    __file__, globals(), [
        decorators.expectedFailureAll(
            oslist=["windows"], bugnumber="llvm.org/pr24764"),
        decorators.expectedFailureAll(bugnumber="<rdar://problem/31811802>")])

