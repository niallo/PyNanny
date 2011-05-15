from nanny import Nanny

def test_nanny_dynamic_calling():
    """ Test the __getattribute__ support in the Nanny object """

    n = Nanny()

    assert n.nanny_child_new("/bin/true")

