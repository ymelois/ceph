# -*- coding: utf-8 -*-
from __future__ import absolute_import

from unittest.mock import MagicMock, patch

import rados
import rbd

from .. import mgr
from ..controllers.rbd import RbdNamespace
from ..tests import ControllerTestCase


class RbdNamespaceControllerTest(ControllerTestCase):

    @classmethod
    def setup_server(cls):
        cls.setup_controllers([RbdNamespace], cp_config={
            'tools.json_in.on': True,
            'tools.json_in.force': False,
        })

    def setUp(self):
        self.mock_ioctx = MagicMock()
        mgr.rados = MagicMock()
        mgr.rados.open_ioctx.return_value = MagicMock()
        mgr.rados.open_ioctx.return_value.__enter__ = MagicMock(
            return_value=self.mock_ioctx)
        mgr.rados.open_ioctx.return_value.__exit__ = MagicMock(
            return_value=False)

        self.mock_rbd_inst = MagicMock()
        self.mock_rbd_inst.namespace_create.return_value = None
        self.mock_rbd_inst.namespace_remove.return_value = None
        self.mock_rbd_inst.namespace_set_quota.return_value = None

        for ep in self._endpoints_cache.get(RbdNamespace, []):
            if hasattr(ep, 'inst'):
                ep.inst.rbd_inst = self.mock_rbd_inst

        # The error handler catches rbd.OSError/rbd.Error, which are Mocks
        # in UNITTEST mode. Using a Mock in an except clause causes TypeError,
        # so replace them with real exception classes.
        rbd.OSError = Exception
        rbd.Error = Exception
        rados.OSError = Exception
        rados.Error = Exception

    @patch('dashboard.controllers.rbd.RbdService.rbd_pool_list')
    def test_list(self, mock_pool_list):
        mock_pool_list.return_value = ([], 0)
        self.mock_rbd_inst.namespace_list.return_value = ['ns1', 'ns2']

        self._get('/api/block/pool/rbd/namespace')
        self.assertStatus(200)
        self.assertJsonBody([
            {'namespace': 'ns1', 'num_images': 0},
            {'namespace': 'ns2', 'num_images': 0},
        ])

    def test_create(self):
        self.mock_rbd_inst.namespace_list.return_value = []

        self._post('/api/block/pool/rbd/namespace', {'namespace': 'ns1'})
        self.assertStatus(201)
        self.mock_rbd_inst.namespace_create.assert_called_with(
            self.mock_ioctx, 'ns1')

    def test_create_already_exists(self):
        self.mock_rbd_inst.namespace_list.return_value = ['ns1']

        self._post('/api/block/pool/rbd/namespace', {'namespace': 'ns1'})
        self.assertStatus(400)

    @patch('dashboard.controllers.rbd.RbdService.rbd_pool_list')
    def test_delete(self, mock_pool_list):
        mock_pool_list.return_value = ([], 0)

        self._delete('/api/block/pool/rbd/namespace/ns1')
        self.assertStatus(204)
        self.mock_rbd_inst.namespace_remove.assert_called_with(
            self.mock_ioctx, 'ns1')

    @patch('dashboard.controllers.rbd.RbdService.rbd_pool_list')
    def test_delete_with_images(self, mock_pool_list):
        mock_pool_list.return_value = ([{'name': 'img1'}], 1)

        self._delete('/api/block/pool/rbd/namespace/ns1')
        self.assertStatus(400)

    def test_get_quota(self):
        quota_info = {
            'max_bytes': 1073741824,
            'max_objects': 1000,
            'used_bytes': 524288,
            'used_objects': 42,
        }
        self.mock_rbd_inst.namespace_get_quota.return_value = quota_info

        self._get('/api/block/pool/rbd/namespace/ns1/quota')
        self.assertStatus(200)
        self.assertJsonBody(quota_info)
        self.mock_rbd_inst.namespace_get_quota.assert_called_with(
            self.mock_ioctx, 'ns1')

    def test_get_quota_unlimited(self):
        quota_info = {
            'max_bytes': 0,
            'max_objects': 0,
            'used_bytes': 0,
            'used_objects': 0,
        }
        self.mock_rbd_inst.namespace_get_quota.return_value = quota_info

        self._get('/api/block/pool/rbd/namespace/ns1/quota')
        self.assertStatus(200)
        self.assertJsonBody(quota_info)

    def test_set_quota_both(self):
        self._put('/api/block/pool/rbd/namespace/ns1/quota', {
            'max_bytes': 1073741824,
            'max_objects': 1000,
        })
        self.assertStatus(200)
        self.mock_rbd_inst.namespace_set_quota.assert_called_with(
            self.mock_ioctx, 'ns1',
            set_max_bytes=True, max_bytes=1073741824,
            set_max_objects=True, max_objects=1000)

    def test_set_quota_bytes_only(self):
        self._put('/api/block/pool/rbd/namespace/ns1/quota', {
            'max_bytes': 1073741824,
        })
        self.assertStatus(200)
        self.mock_rbd_inst.namespace_set_quota.assert_called_with(
            self.mock_ioctx, 'ns1',
            set_max_bytes=True, max_bytes=1073741824,
            set_max_objects=False, max_objects=0)

    def test_set_quota_objects_only(self):
        self._put('/api/block/pool/rbd/namespace/ns1/quota', {
            'max_objects': 500,
        })
        self.assertStatus(200)
        self.mock_rbd_inst.namespace_set_quota.assert_called_with(
            self.mock_ioctx, 'ns1',
            set_max_bytes=False, max_bytes=0,
            set_max_objects=True, max_objects=500)

    def test_set_quota_no_fields(self):
        self._put('/api/block/pool/rbd/namespace/ns1/quota', {})
        self.assertStatus(400)
