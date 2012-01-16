/*
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2012 Rob Jansen <jansen@cs.umn.edu>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shadow.h"

Internetwork* internetwork_new() {
	Internetwork* internet = g_new0(Internetwork, 1);
	MAGIC_INIT(internet);

	internet->nodes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, node_free);
	internet->networks = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, network_free);
	internet->networksByIP = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, NULL);
	internet->ipByName = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	internet->nameByIp = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, g_free);

	return internet;
}

void internetwork_free(Internetwork* internet) {
	MAGIC_ASSERT(internet);

	/* free all applications before freeing any of the nodes since freeing
	 * applications may cause close() to get called on sockets which needs
	 * other node information.
	 */
	g_hash_table_foreach(internet->nodes, node_stopApplication, NULL);

	/* now cleanup the rest */
	g_hash_table_destroy(internet->nodes);
	g_hash_table_destroy(internet->networks);
	g_hash_table_destroy(internet->networksByIP);
	g_hash_table_destroy(internet->ipByName);
	g_hash_table_destroy(internet->nameByIp);

	MAGIC_CLEAR(internet);
	g_free(internet);
}

static void _internetwork_trackLatency(Internetwork* internet, Link* link) {
	MAGIC_ASSERT(internet);

	guint64 latency = link_getLatency(link);
	guint64 jitter = link_getJitter(link);

	internet->maximumGlobalLatency = MAX(internet->maximumGlobalLatency, (latency+jitter));
	internet->minimumGlobalLatency = MIN(internet->minimumGlobalLatency, (latency-jitter));
}

void internetwork_createNetwork(Internetwork* internet, GQuark networkID, guint64 bandwidthdown, guint64 bandwidthup) {
	MAGIC_ASSERT(internet);
	g_assert(!internet->isReadOnly);

	Network* network = network_new(networkID, bandwidthdown, bandwidthup);
	g_hash_table_replace(internet->networks, network_getIDReference(network), network);
}

void internetwork_connectNetworks(Internetwork* internet,
		GQuark sourceClusterID, GQuark destinationClusterID,
		guint64 latency, guint64 jitter, gdouble packetloss) {
	MAGIC_ASSERT(internet);
	g_assert(!internet->isReadOnly);

	/* lookup our networks */
	Network* sourceNetwork = internetwork_getNetwork(internet, sourceClusterID);
	Network* destinationNetwork = internetwork_getNetwork(internet, destinationClusterID);
	g_assert(sourceNetwork && destinationNetwork);

	/* create the link */
	Link* link = link_new(sourceNetwork, destinationNetwork, latency, jitter, packetloss);

	/* build links into topology */
	network_addOutgoingLink(sourceNetwork, link);
	network_addIncomingLink(destinationNetwork, link);

	/* track latency */
	_internetwork_trackLatency(internet, link);
}

Network* internetwork_getNetwork(Internetwork* internet, GQuark networkID) {
	MAGIC_ASSERT(internet);
	return (Network*) g_hash_table_lookup(internet->networks, &networkID);
}

Network* internetwork_getRandomNetwork(Internetwork* internet) {
	MAGIC_ASSERT(internet);

	/* TODO this is ugly.
	 * I cant believe the g_list iterates the list to count the length...
	 */

	GList* networkList = g_hash_table_get_values(internet->networks);
	guint length = g_list_length(networkList);

	gdouble r = random_nextDouble(worker_getPrivate()->random);
	guint n = (guint)(((gdouble)length) * r);
	g_assert((n >= 0) && (n <= length));

	Network* network = (Network*) g_list_nth_data(networkList, n);
	g_list_free(networkList);

	return network;
}

Network* internetwork_lookupNetwork(Internetwork* internet, in_addr_t ip) {
	MAGIC_ASSERT(internet);
	return (Network*) g_hash_table_lookup(internet->networksByIP, &ip);
}

static guint32 _internetwork_generateIP(Internetwork* internet) {
	MAGIC_ASSERT(internet);
	internet->ipCounter++;
	while(internet->ipCounter == htonl(INADDR_NONE) ||
			internet->ipCounter == htonl(INADDR_ANY) ||
			internet->ipCounter == htonl(INADDR_LOOPBACK) ||
			internet->ipCounter == htonl(INADDR_BROADCAST))
	{
		internet->ipCounter++;
	}
	return internet->ipCounter;
}

void internetwork_createNode(Internetwork* internet, GQuark nodeID,
		Network* network, Software* software, GString* hostname,
		guint64 bwDownKiBps, guint64 bwUpKiBps, guint64 cpuBps) {
	MAGIC_ASSERT(internet);
	g_assert(!internet->isReadOnly);

	guint32 ip = _internetwork_generateIP(internet);
	ip = (guint32) nodeID;
	Node* node = node_new(nodeID, network, software, ip, hostname, bwDownKiBps, bwUpKiBps, cpuBps);
	g_hash_table_replace(internet->nodes, GUINT_TO_POINTER((guint)nodeID), node);


	gchar* mapName = g_strdup((const gchar*) hostname->str);
	guint32* mapIP = g_new0(guint32, 1);
	*mapIP = ip;
	g_hash_table_replace(internet->networksByIP, mapIP, network);
	g_hash_table_replace(internet->ipByName, mapName, mapIP);
	g_hash_table_replace(internet->nameByIp, mapIP, mapName);
}

Node* internetwork_getNode(Internetwork* internet, GQuark nodeID) {
	MAGIC_ASSERT(internet);
	return (Node*) g_hash_table_lookup(internet->nodes, GUINT_TO_POINTER((guint)nodeID));
}

GList* internetwork_getAllNodes(Internetwork* internet) {
	MAGIC_ASSERT(internet);
	return g_hash_table_get_values(internet->nodes);
}

guint32 internetwork_resolveName(Internetwork* internet, gchar* name) {
	MAGIC_ASSERT(internet);
	return g_quark_try_string((const gchar*) name);
//	guint32* ip = g_hash_table_lookup(internet->ipByName, name);
//	if(ip) {
//		return *ip;
//	} else {
//		return (guint32)INADDR_NONE;
//	}
}

const gchar* internetwork_resolveIP(Internetwork* internet, guint32 ip) {
	MAGIC_ASSERT(internet);
	return g_hash_table_lookup(internet->nameByIp, &ip);
}

const gchar* internetwork_resolveID(Internetwork* internet, GQuark id) {
	MAGIC_ASSERT(internet);
	return g_quark_to_string(id);
}

gdouble internetwork_getMaximumGlobalLatency(Internetwork* internet) {
	MAGIC_ASSERT(internet);
	return internet->maximumGlobalLatency;
}

gdouble internetwork_getMinimumGlobalLatency(Internetwork* internet) {
	MAGIC_ASSERT(internet);
	return internet->minimumGlobalLatency;
}

guint32 internetwork_getNodeBandwidthUp(Internetwork* internet, GQuark nodeID) {
	MAGIC_ASSERT(internet);
	Node* node = internetwork_getNode(internet, nodeID);
	NetworkInterface* interface = node_lookupInterface(node, nodeID);
	return networkinterface_getSpeedUpKiBps(interface);
}

guint32 internetwork_getNodeBandwidthDown(Internetwork* internet, GQuark nodeID) {
	MAGIC_ASSERT(internet);
	Node* node = internetwork_getNode(internet, nodeID);
	NetworkInterface* interface = node_lookupInterface(node, nodeID);
	return networkinterface_getSpeedDownKiBps(interface);
}

gdouble internetwork_getReliability(Internetwork* internet, GQuark sourceNodeID, GQuark destinationNodeID) {
	MAGIC_ASSERT(internet);
	Node* sourceNode = internetwork_getNode(internet, sourceNodeID);
	Network* sourceNetwork = node_getNetwork(sourceNode);
	Node* destinationNode = internetwork_getNode(internet, destinationNodeID);
	Network* destinationNetwork = node_getNetwork(destinationNode);
	return network_getLinkReliability(sourceNetwork, destinationNetwork);
}

gdouble internetwork_getLatency(Internetwork* internet, GQuark sourceNodeID, GQuark destinationNodeID, gdouble percentile) {
	MAGIC_ASSERT(internet);
	Node* sourceNode = internetwork_getNode(internet, sourceNodeID);
	Network* sourceNetwork = node_getNetwork(sourceNode);
	Node* destinationNode = internetwork_getNode(internet, destinationNodeID);
	Network* destinationNetwork = node_getNetwork(destinationNode);
	return network_getLinkLatency(sourceNetwork, destinationNetwork, percentile);
}

gdouble internetwork_sampleLatency(Internetwork* internet, GQuark sourceNodeID, GQuark destinationNodeID) {
	MAGIC_ASSERT(internet);
	Node* sourceNode = internetwork_getNode(internet, sourceNodeID);
	Network* sourceNetwork = node_getNetwork(sourceNode);
	Node* destinationNode = internetwork_getNode(internet, destinationNodeID);
	Network* destinationNetwork = node_getNetwork(destinationNode);
	return network_sampleLinkLatency(sourceNetwork, destinationNetwork);
}
