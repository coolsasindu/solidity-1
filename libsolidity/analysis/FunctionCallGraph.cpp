/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libsolidity/analysis/FunctionCallGraph.h>

#include <range/v3/view/reverse.hpp>
#include <range/v3/view/transform.hpp>

using namespace std;
using namespace ranges;
using namespace solidity::frontend;

FunctionCallGraphBuilder::FunctionCallGraphBuilder(ContractDefinition const& _contract):
	m_contract(&_contract),
	m_graph(make_unique<ContractCallGraph>(_contract))
{
	// Create graph for constructor, state vars, etc
	m_currentNode = SpecialNode::EntryCreation;
	m_currentDispatch = SpecialNode::InternalCreationDispatch;

	for (ContractDefinition const* contract: _contract.annotation().linearizedBaseContracts | views::reverse)
	{
		for (auto const* stateVar: contract->stateVariables())
			stateVar->accept(*this);

		for (auto arg: contract->baseContracts())
			arg->accept(*this);

		if (contract->constructor())
		{
			add(*m_currentNode, contract->constructor());
			contract->constructor()->accept(*this);
			m_currentNode = contract->constructor();
		}
	}

	m_currentNode.reset();
	m_currentDispatch = SpecialNode::InternalDispatch;

	auto getSecondElement = [](auto const& _tuple){ return get<1>(_tuple); };

	// Create graph for all publicly reachable functions
	for (FunctionTypePointer functionType: _contract.interfaceFunctionList() | views::transform(getSecondElement))
	{
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(&functionType->declaration()))
		{
			if (!m_graph->edges.count(funcDef))
				visitCallable(funcDef);

			// Add all external functions to the RuntimeDispatch
			add(SpecialNode::Entry, &functionType->declaration());
		}
		else
			// If it's not a function, it must be a getter of a public variable; we ignore those
			solAssert(dynamic_cast<VariableDeclaration const*>(&functionType->declaration()), "");
	}

	// Add all InternalCreationDispatch calls to the InternalDispatch as well
	add(SpecialNode::InternalDispatch, SpecialNode::InternalCreationDispatch);

	if (_contract.fallbackFunction())
		add(SpecialNode::Entry, _contract.fallbackFunction());

	if (_contract.receiveFunction())
		add(SpecialNode::Entry, _contract.receiveFunction());

	m_currentNode.reset();
}

bool FunctionCallGraphBuilder::CompareByID::operator()(Node const& _lhs, Node const& _rhs) const
{
	if (_lhs.index() != _rhs.index())
		return _lhs.index() < _rhs.index();

	if (holds_alternative<SpecialNode>(_lhs))
		return get<SpecialNode>(_lhs) < get<SpecialNode>(_rhs);
	return get<ASTNode const*>(_lhs)->id() < get<ASTNode const*>(_rhs)->id();
}

bool FunctionCallGraphBuilder::CompareByID::operator()(Node const& _lhs, int64_t _rhs) const
{
	solAssert(!holds_alternative<SpecialNode>(_lhs), "");

	return get<ASTNode const*>(_lhs)->id() < _rhs;
}

bool FunctionCallGraphBuilder::CompareByID::operator()(int64_t _lhs, Node const& _rhs) const
{
	solAssert(!holds_alternative<SpecialNode>(_rhs), "");

	return _lhs < get<ASTNode const*>(_rhs)->id();
}

unique_ptr<FunctionCallGraphBuilder::ContractCallGraph> FunctionCallGraphBuilder::create(ContractDefinition const& _contract)
{
	return FunctionCallGraphBuilder(_contract).m_graph;
}

bool FunctionCallGraphBuilder::visit(Identifier const& _identifier)
{
	if (auto const* callable = dynamic_cast<CallableDeclaration const*>(_identifier.annotation().referencedDeclaration))
	{
		solAssert(*_identifier.annotation().requiredLookup == VirtualLookup::Virtual, "");

		auto funType = dynamic_cast<FunctionType const*>(_identifier.annotation().type);

		// For events kind() == Event, so we have an extra check here
		if (funType && funType->kind() == FunctionType::Kind::Internal)
		{
			processFunction(callable->resolveVirtual(*m_contract), _identifier.annotation().calledDirectly);

			solAssert(m_currentNode.has_value(), "");
		}
	}

	return true;
}

bool FunctionCallGraphBuilder::visit(NewExpression const& _newExpression)
{
	if (ContractType const* contractType = dynamic_cast<ContractType const*>(_newExpression.typeName().annotation().type))
		m_graph->createdContracts.emplace(&contractType->contractDefinition());

	return true;
}

void FunctionCallGraphBuilder::endVisit(MemberAccess const& _memberAccess)
{
	auto functionType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type);
	auto functionDef = dynamic_cast<FunctionDefinition const*>(_memberAccess.annotation().referencedDeclaration);
	if (!functionType || !functionDef || functionType->kind() != FunctionType::Kind::Internal)
		return;

	// Super functions
	if (*_memberAccess.annotation().requiredLookup == VirtualLookup::Super)
	{
		if (auto const* typeType = dynamic_cast<TypeType const*>(_memberAccess.expression().annotation().type))
			if (auto const contractType = dynamic_cast<ContractType const*>(typeType->actualType()))
			{
				solAssert(contractType->isSuper(), "");
				functionDef =
					&functionDef->resolveVirtual(
						*m_contract,
						contractType->contractDefinition().superContract(*m_contract)
					);
			}
	}
	else
		solAssert(*_memberAccess.annotation().requiredLookup == VirtualLookup::Static, "");

	processFunction(*functionDef, _memberAccess.annotation().calledDirectly);
	return;
}

void FunctionCallGraphBuilder::endVisit(ModifierInvocation const& _modifierInvocation)
{
	VirtualLookup const& requiredLookup = *_modifierInvocation.name().annotation().requiredLookup;

	if (auto const* modifier = dynamic_cast<ModifierDefinition const*>(_modifierInvocation.name().annotation().referencedDeclaration))
	{
		if (requiredLookup == VirtualLookup::Virtual)
			modifier = &modifier->resolveVirtual(*m_contract);
		else
			solAssert(requiredLookup == VirtualLookup::Static, "");

		processFunction(*modifier);
	}
}

void FunctionCallGraphBuilder::visitCallable(CallableDeclaration const* _callable)
{
	solAssert(!m_graph->edges.count(_callable), "");

	optional<Node> previousNode = m_currentNode;
	m_currentNode = _callable;

	_callable->accept(*this);

	m_currentNode = previousNode;
}

bool FunctionCallGraphBuilder::add(Node _caller, Node _callee)
{
	return m_graph->edges[_caller].insert(_callee).second;
}

void FunctionCallGraphBuilder::processFunction(CallableDeclaration const& _callable, bool _calledDirectly)
{
	if (m_currentNode.has_value() && _calledDirectly)
		add(*m_currentNode, &_callable);

	if (!_calledDirectly)
	{
		add(m_currentDispatch, &_callable);

		add(&_callable, m_currentDispatch);
	}

	if (!m_graph->edges.count(&_callable))
		visitCallable(&_callable);
}