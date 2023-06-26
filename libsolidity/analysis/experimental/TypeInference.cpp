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


#include <libsolidity/analysis/experimental/TypeInference.h>
#include <libsolidity/analysis/experimental/TypeRegistration.h>
#include <libsolidity/analysis/experimental/Analysis.h>
#include <libsolidity/ast/experimental/TypeSystemHelper.h>

#include <libsolutil/Numeric.h>
#include <libsolutil/StringUtils.h>
#include <liblangutil/Exceptions.h>

#include <libyul/AsmAnalysis.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/AST.h>

#include <boost/algorithm/string.hpp>
#include <range/v3/view/transform.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::frontend;
using namespace solidity::frontend::experimental;
using namespace solidity::langutil;

TypeInference::TypeInference(Analysis& _analysis):
m_analysis(_analysis),
m_errorReporter(_analysis.errorReporter()),
m_typeSystem(_analysis.typeSystem())
{
	m_voidType = m_typeSystem.type(PrimitiveType::Void, {});
	m_wordType = m_typeSystem.type(PrimitiveType::Word, {});
	m_integerType = m_typeSystem.type(PrimitiveType::Integer, {});
	m_unitType = m_typeSystem.type(PrimitiveType::Unit, {});
	m_boolType = m_typeSystem.type(PrimitiveType::Bool, {});
	m_env = &m_typeSystem.env();
}

bool TypeInference::analyze(SourceUnit const& _sourceUnit)
{
	_sourceUnit.accept(*this);
	return !m_errorReporter.hasErrors();
}

bool TypeInference::visit(FunctionDefinition const& _functionDefinition)
{
	solAssert(m_expressionContext == ExpressionContext::Term);
	auto& functionAnnotation = annotation(_functionDefinition);
	if (functionAnnotation.type)
		return false;

	ScopedSaveAndRestore signatureRestore(m_currentFunctionType, nullopt);

	_functionDefinition.parameterList().accept(*this);
	if (_functionDefinition.returnParameterList())
		_functionDefinition.returnParameterList()->accept(*this);

	auto getListType = [&](ParameterList const* _list) { return _list ? getType(*_list) : m_unitType; };
	Type functionType = TypeSystemHelpers{m_typeSystem}.functionType(
		getListType(&_functionDefinition.parameterList()),
		getListType(_functionDefinition.returnParameterList().get())
	);

	m_currentFunctionType = functionType;

	if (_functionDefinition.isImplemented())
		_functionDefinition.body().accept(*this);

	functionAnnotation.type = functionType;
	return false;
}

void TypeInference::endVisit(Return const& _return)
{
	solAssert(m_currentFunctionType);
	Type functionReturnType = get<1>(TypeSystemHelpers{m_typeSystem}.destFunctionType(*m_currentFunctionType));
	if (_return.expression())
		unify(functionReturnType, getType(*_return.expression()), _return.location());
	else
		unify(functionReturnType, m_unitType, _return.location());
}

void TypeInference::endVisit(ParameterList const& _parameterList)
{
	auto& listAnnotation = annotation(_parameterList);
	solAssert(!listAnnotation.type);
	listAnnotation.type = TypeSystemHelpers{m_typeSystem}.tupleType(
		_parameterList.parameters() | ranges::views::transform([&](auto _arg) { return getType(*_arg); }) | ranges::to<vector<Type>>
	);
}

bool TypeInference::visit(TypeClassDefinition const& _typeClassDefinition)
{
	solAssert(m_expressionContext == ExpressionContext::Term);
	auto& typeClassAnnotation = annotation(_typeClassDefinition);
	if (typeClassAnnotation.type)
		return false;
	typeClassAnnotation.type = type(&_typeClassDefinition, {});
	{
		ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Type};
		_typeClassDefinition.typeVariable().accept(*this);
	}

	map<string, Type> functionTypes;

	Type typeVar = m_typeSystem.freshTypeVariable({});

	auto& typeMembers = annotation().members[typeConstructor(&_typeClassDefinition)];

	for (auto subNode: _typeClassDefinition.subNodes())
	{
		subNode->accept(*this);
		auto const* functionDefinition = dynamic_cast<FunctionDefinition const*>(subNode.get());
		solAssert(functionDefinition);
		auto functionType = m_env->fresh(getType(*functionDefinition));
		functionTypes[functionDefinition->name()] = functionType;
		auto typeVars = TypeEnvironmentHelpers{*m_env}.typeVars(functionType);
		if(typeVars.size() != 1)
			m_errorReporter.fatalTypeError(0000_error, functionDefinition->location(), "Function in type class may only depend on the type class variable.");
		unify(typeVars.front(), typeVar, functionDefinition->location());

		if (!typeMembers.emplace(functionDefinition->name(), TypeMember{functionType}).second)
			m_errorReporter.fatalTypeError(0000_error, functionDefinition->location(), "Function in type class declared multiple times.");
	}

	TypeClass typeClass = std::visit(util::GenericVisitor{
		[](TypeClass _class) -> TypeClass { return _class; },
		[&](std::string _error) -> TypeClass {
			m_errorReporter.fatalTypeError(0000_error, _typeClassDefinition.location(), _error);
			util::unreachable();
		}
	}, m_typeSystem.declareTypeClass(typeVar, std::move(functionTypes), _typeClassDefinition.name(), &_typeClassDefinition));


	unify(getType(_typeClassDefinition.typeVariable()), m_typeSystem.freshTypeVariable({{typeClass}}), _typeClassDefinition.location());
	for (auto instantiation: m_analysis.annotation<TypeRegistration>(_typeClassDefinition).instantiations | ranges::views::values)
		// TODO: recursion-safety? Order of instantiation?
		instantiation->accept(*this);

	return false;
}

bool TypeInference::visit(InlineAssembly const& _inlineAssembly)
{
	// External references have already been resolved in a prior stage and stored in the annotation.
	// We run the resolve step again regardless.
	yul::ExternalIdentifierAccess::Resolver identifierAccess = [&](
		yul::Identifier const& _identifier,
		yul::IdentifierContext _context,
		bool
	) -> bool
	{
		if (_context == yul::IdentifierContext::NonExternal)
		{
			// TODO: do we need this?
			// Hack until we can disallow any shadowing: If we found an internal reference,
			// clear the external references, so that codegen does not use it.
			_inlineAssembly.annotation().externalReferences.erase(& _identifier);
			return false;
		}
		InlineAssemblyAnnotation::ExternalIdentifierInfo* identifierInfo = util::valueOrNullptr(_inlineAssembly.annotation().externalReferences, &_identifier);
		if (!identifierInfo)
			return false;
		Declaration const* declaration = identifierInfo->declaration;
		solAssert(!!declaration, "");
		solAssert(identifierInfo->suffix == "", "");

		unify(getType(*declaration), m_wordType, originLocationOf(_identifier));
		identifierInfo->valueSize = 1;
		return true;
	};
	solAssert(!_inlineAssembly.annotation().analysisInfo, "");
	_inlineAssembly.annotation().analysisInfo = make_shared<yul::AsmAnalysisInfo>();
	yul::AsmAnalyzer analyzer(
		*_inlineAssembly.annotation().analysisInfo,
		m_errorReporter,
		_inlineAssembly.dialect(),
		identifierAccess
	);
	if (!analyzer.analyze(_inlineAssembly.operations()))
		solAssert(m_errorReporter.hasErrors());
	return false;
}

bool TypeInference::visit(ElementaryTypeNameExpression const& _expression)
{
	auto& expressionAnnotation = annotation(_expression);
	solAssert(!expressionAnnotation.type);

	if (m_expressionContext != ExpressionContext::Type)
	{
		m_errorReporter.typeError(0000_error, _expression.location(), "Elementary type name expression only supported in type context.");
		expressionAnnotation.type = m_typeSystem.freshTypeVariable({});
		return false;
	}

	if (auto constructor = m_analysis.annotation<TypeRegistration>(_expression).typeConstructor)
	{
		vector<Type> arguments;
		std::generate_n(std::back_inserter(arguments), m_typeSystem.constructorInfo(*constructor).arguments(), [&]() {
			return m_typeSystem.freshTypeVariable({});
		});
		if (arguments.empty())
			expressionAnnotation.type = m_typeSystem.type(*constructor, arguments);
		else
		{
			TypeSystemHelpers helper{m_typeSystem};
			expressionAnnotation.type =
				helper.typeFunctionType(
					helper.tupleType(arguments),
					m_typeSystem.type(*constructor, arguments)
				);
		}
	}
	else
	{
		m_errorReporter.typeError(0000_error, _expression.location(), "No type constructor registered for elementary type name.");
		expressionAnnotation.type = m_typeSystem.freshTypeVariable({});
	}
	return false;
}

bool TypeInference::visit(BinaryOperation const& _binaryOperation)
{
	auto& operationAnnotation = annotation(_binaryOperation);
	solAssert(!operationAnnotation.type);
	TypeSystemHelpers helper{m_typeSystem};
	switch (m_expressionContext)
	{
	case ExpressionContext::Term:
		if (auto* operatorInfo = util::valueOrNullptr(m_analysis.annotation<TypeRegistration>().operators, _binaryOperation.getOperator()))
		{
			auto [typeClass, functionName] = *operatorInfo;
			optional<Type> functionType = m_env->typeClassFunction(typeClass, functionName);
			solAssert(functionType);

			_binaryOperation.leftExpression().accept(*this);
			_binaryOperation.rightExpression().accept(*this);

			Type argTuple = helper.tupleType({getType(_binaryOperation.leftExpression()), getType(_binaryOperation.rightExpression())});
			Type genericFunctionType = helper.functionType(argTuple, m_typeSystem.freshTypeVariable({}));
			unify(*functionType, genericFunctionType, _binaryOperation.location());

			operationAnnotation.type = m_env->resolve(std::get<1>(helper.destFunctionType(m_env->resolve(genericFunctionType))));

			return false;
		}
		else
		{
			m_errorReporter.typeError(0000_error, _binaryOperation.location(), "Binary operation in term context not yet supported.");
			operationAnnotation.type = m_typeSystem.freshTypeVariable({});
			return false;
		}
	case ExpressionContext::Type:
		if (_binaryOperation.getOperator() == Token::Colon)
		{
			_binaryOperation.leftExpression().accept(*this);
			{
				ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Sort};
				_binaryOperation.rightExpression().accept(*this);
			}
			Type leftType = getType(_binaryOperation.leftExpression());
			unify(leftType, getType(_binaryOperation.rightExpression()), _binaryOperation.location());
			operationAnnotation.type = leftType;
		}
		else if (_binaryOperation.getOperator() == Token::RightArrow)
		{
			_binaryOperation.leftExpression().accept(*this);
			_binaryOperation.rightExpression().accept(*this);
			operationAnnotation.type = helper.functionType(getType(_binaryOperation.leftExpression()), getType(_binaryOperation.rightExpression()));
		}
		else
		{
			m_errorReporter.typeError(0000_error, _binaryOperation.location(), "Invalid binary operations in type context.");
			operationAnnotation.type = m_typeSystem.freshTypeVariable({});
		}
		return false;
	case ExpressionContext::Sort:
		m_errorReporter.typeError(0000_error, _binaryOperation.location(), "Invalid binary operation in sort context.");
		operationAnnotation.type = m_typeSystem.freshTypeVariable({});
		return false;
	}
	return false;
}

void TypeInference::endVisit(VariableDeclarationStatement const& _variableDeclarationStatement)
{
	solAssert(m_expressionContext == ExpressionContext::Term);
	if (_variableDeclarationStatement.declarations().size () != 1)
	{
		m_errorReporter.typeError(0000_error, _variableDeclarationStatement.location(), "Multi variable declaration not supported.");
		return;
	}
	Type variableType = getType(*_variableDeclarationStatement.declarations().front());
	if (_variableDeclarationStatement.initialValue())
		unify(variableType, getType(*_variableDeclarationStatement.initialValue()), _variableDeclarationStatement.location());
}

bool TypeInference::visit(VariableDeclaration const& _variableDeclaration)
{
	solAssert(!_variableDeclaration.value());
	auto& variableAnnotation = annotation(_variableDeclaration);
	solAssert(!variableAnnotation.type);

	switch (m_expressionContext)
	{
	case ExpressionContext::Term:
		if (_variableDeclaration.typeExpression())
		{
			ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Type};
			_variableDeclaration.typeExpression()->accept(*this);
			variableAnnotation.type = getType(*_variableDeclaration.typeExpression());
			return false;
		}
		variableAnnotation.type = m_typeSystem.freshTypeVariable({});
		return false;
	case ExpressionContext::Type:
		variableAnnotation.type = m_typeSystem.freshTypeVariable({});
		if (_variableDeclaration.typeExpression())
		{
			ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Sort};
			_variableDeclaration.typeExpression()->accept(*this);
			unify(*variableAnnotation.type, getType(*_variableDeclaration.typeExpression()), _variableDeclaration.typeExpression()->location());
		}
		return false;
	case ExpressionContext::Sort:
		m_errorReporter.typeError(0000_error, _variableDeclaration.location(), "Variable declaration in sort context.");
		variableAnnotation.type = m_typeSystem.freshTypeVariable({});
		return false;
	}
	util::unreachable();
}

void TypeInference::endVisit(Assignment const& _assignment)
{
	auto& assignmentAnnotation = annotation(_assignment);
	solAssert(!assignmentAnnotation.type);

	if (m_expressionContext != ExpressionContext::Term)
	{
		m_errorReporter.typeError(0000_error, _assignment.location(), "Assignment outside term context.");
		assignmentAnnotation.type = m_typeSystem.freshTypeVariable({});
		return;
	}

	Type leftType = getType(_assignment.leftHandSide());
	unify(leftType, getType(_assignment.rightHandSide()), _assignment.location());
	assignmentAnnotation.type = m_env->resolve(leftType);
}

experimental::Type TypeInference::handleIdentifierByReferencedDeclaration(langutil::SourceLocation _location, Declaration const& _declaration)
{
	switch(m_expressionContext)
	{
	case ExpressionContext::Term:
	{
		if (
			!dynamic_cast<FunctionDefinition const*>(&_declaration) &&
			!dynamic_cast<VariableDeclaration const*>(&_declaration) &&
			!dynamic_cast<TypeClassDefinition const*>(&_declaration) &&
			!dynamic_cast<TypeDefinition const*>(&_declaration)
		)
		{
			SecondarySourceLocation ssl;
			ssl.append("Referenced node.", _declaration.location());
			m_errorReporter.fatalTypeError(0000_error, _location, ssl, "Attempt to type identifier referring to unexpected node.");
		}

		auto& declarationAnnotation = annotation(_declaration);
		if (!declarationAnnotation.type)
			_declaration.accept(*this);

		solAssert(declarationAnnotation.type);

		if (dynamic_cast<VariableDeclaration const*>(&_declaration))
			return *declarationAnnotation.type;
		else if (dynamic_cast<FunctionDefinition const*>(&_declaration))
			return m_env->fresh(*declarationAnnotation.type);
		else if (dynamic_cast<TypeClassDefinition const*>(&_declaration))
			return m_env->fresh(*declarationAnnotation.type);
		else if (dynamic_cast<TypeDefinition const*>(&_declaration))
			return m_env->fresh(*declarationAnnotation.type);
		else
			solAssert(false);
		break;
	}
	case ExpressionContext::Type:
	{
		if (
			!dynamic_cast<VariableDeclaration const*>(&_declaration) &&
			!dynamic_cast<TypeDefinition const*>(&_declaration)
		)
		{
			SecondarySourceLocation ssl;
			ssl.append("Referenced node.", _declaration.location());
			m_errorReporter.fatalTypeError(0000_error, _location, ssl, "Attempt to type identifier referring to unexpected node.");
		}

		// TODO: Assert that this is a type class variable declaration?
		auto& declarationAnnotation = annotation(_declaration);
		if (!declarationAnnotation.type)
			_declaration.accept(*this);

		solAssert(declarationAnnotation.type);

		if (dynamic_cast<VariableDeclaration const*>(&_declaration))
			return *declarationAnnotation.type;
		else if (dynamic_cast<TypeDefinition const*>(&_declaration))
			return m_env->fresh(*declarationAnnotation.type);
		else
			solAssert(false);
		break;
	}
	case ExpressionContext::Sort:
	{
		if (auto const* typeClass = dynamic_cast<TypeClassDefinition const*>(&_declaration))
		{
			ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Term};
			typeClass->accept(*this);
			if (!annotation(*typeClass).typeClass)
			{
				m_errorReporter.typeError(0000_error, _location, "Unregistered type class.");
				return m_typeSystem.freshTypeVariable({});
			}
			return m_typeSystem.freshTypeVariable(Sort{{*annotation(*typeClass).typeClass}});
		}
		else
		{
			m_errorReporter.typeError(0000_error, _location, "Expected type class.");
			return m_typeSystem.freshTypeVariable({});
		}
		break;
	}
	}
	util::unreachable();
}

bool TypeInference::visit(Identifier const& _identifier)
{
	auto& identifierAnnotation = annotation(_identifier);
	solAssert(!identifierAnnotation.type);

	if (auto const* referencedDeclaration = _identifier.annotation().referencedDeclaration)
	{
		identifierAnnotation.type = handleIdentifierByReferencedDeclaration(_identifier.location(), *referencedDeclaration);
		return false;
	}

	switch(m_expressionContext)
	{
	case ExpressionContext::Term:
		// TODO: error handling
		solAssert(false);
		break;
	case ExpressionContext::Type:
		// TODO: register free type variable name!
		identifierAnnotation.type = m_typeSystem.freshTypeVariable({});
		return false;
	case ExpressionContext::Sort:
		// TODO: error handling
		solAssert(false);
		break;
	}

	return false;
}

void TypeInference::endVisit(TupleExpression const& _tupleExpression)
{
	auto& expressionAnnotation = annotation(_tupleExpression);
	solAssert(!expressionAnnotation.type);

	TypeSystemHelpers helper{m_typeSystem};
	auto componentTypes = _tupleExpression.components() | ranges::views::transform([&](auto _expr) -> Type {
		auto& componentAnnotation = annotation(*_expr);
		solAssert(componentAnnotation.type);
		return *componentAnnotation.type;
	}) | ranges::to<vector<Type>>;
	switch (m_expressionContext)
	{
	case ExpressionContext::Term:
	case ExpressionContext::Type:
		expressionAnnotation.type = helper.tupleType(componentTypes);
		break;
	case ExpressionContext::Sort:
	{
		Type type = m_typeSystem.freshTypeVariable({});
		for (auto componentType: componentTypes)
			unify(type, componentType, _tupleExpression.location());
		expressionAnnotation.type = type;
		break;
	}
	}
}

bool TypeInference::visit(IdentifierPath const& _identifierPath)
{
	auto& identifierAnnotation = annotation(_identifierPath);
	solAssert(!identifierAnnotation.type);

	if (auto const* referencedDeclaration = _identifierPath.annotation().referencedDeclaration)
	{
		identifierAnnotation.type = handleIdentifierByReferencedDeclaration(_identifierPath.location(), *referencedDeclaration);
		return false;
	}

	// TODO: error handling
	solAssert(false);
}

bool TypeInference::visit(TypeClassInstantiation const& _typeClassInstantiation)
{
	ScopedSaveAndRestore activeInstantiations{m_activeInstantiations, m_activeInstantiations + set<TypeClassInstantiation const*>{&_typeClassInstantiation}};
	// Note: recursion is resolved due to special handling during unification.
	auto& instantiationAnnotation = annotation(_typeClassInstantiation);
	if (instantiationAnnotation.type)
		return false;
	instantiationAnnotation.type = m_voidType;
	std::optional<TypeClass> typeClass = std::visit(util::GenericVisitor{
		[&](ASTPointer<IdentifierPath> _typeClassName) -> std::optional<TypeClass> {
			if (auto const* typeClass = dynamic_cast<TypeClassDefinition const*>(_typeClassName->annotation().referencedDeclaration))
			{
				// visiting the type class will re-visit this instantiation
				typeClass->accept(*this);
				// TODO: more error handling? Should be covered by the visit above.
				return annotation(*typeClass).typeClass;
			}
			else
			{
				m_errorReporter.typeError(0000_error, _typeClassInstantiation.typeClass().location(), "Expected type class.");
				return nullopt;
			}
		},
		[&](Token _token) -> std::optional<TypeClass> {
			if (auto builtinClass = builtinClassFromToken(_token))
				if (auto typeClass = util::valueOrNullptr(m_analysis.annotation<TypeRegistration>().builtinClasses, *builtinClass))
					return *typeClass;
			m_errorReporter.typeError(0000_error, _typeClassInstantiation.location(), "Invalid type class name.");
			return nullopt;
		}
	}, _typeClassInstantiation.typeClass().name());
	if (!typeClass)
		return false;

	// TODO: _typeClassInstantiation.typeConstructor().accept(*this); ?
	auto typeConstructor = m_analysis.annotation<TypeRegistration>(_typeClassInstantiation.typeConstructor()).typeConstructor;
	if (!typeConstructor)
	{
		m_errorReporter.typeError(0000_error, _typeClassInstantiation.typeConstructor().location(), "Invalid type constructor.");
		return false;
	}

	vector<Type> arguments;
	Arity arity{
		{},
		*typeClass
	};

	{
		ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Type};
		if (_typeClassInstantiation.argumentSorts())
		{
			_typeClassInstantiation.argumentSorts()->accept(*this);
			auto& argumentSortAnnotation = annotation(*_typeClassInstantiation.argumentSorts());
			solAssert(argumentSortAnnotation.type);
			arguments = TypeSystemHelpers{m_typeSystem}.destTupleType(*argumentSortAnnotation.type);
			arity.argumentSorts = arguments | ranges::views::transform([&](Type _type) {
				return m_env->sort(_type);
			}) | ranges::to<vector<Sort>>;
		}
	}

	Type type{TypeConstant{*typeConstructor, arguments}};

	map<string, Type> functionTypes;

	for (auto subNode: _typeClassInstantiation.subNodes())
	{
		auto const* functionDefinition = dynamic_cast<FunctionDefinition const*>(subNode.get());
		solAssert(functionDefinition);
		subNode->accept(*this);
		if (!functionTypes.emplace(functionDefinition->name(), getType(*functionDefinition)).second)
			m_errorReporter.typeError(0000_error, subNode->location(), "Duplicate definition of function " + functionDefinition->name() + " during type class instantiation.");
	}

	if (auto error = m_typeSystem.instantiateClass(type, arity, std::move(functionTypes)))
		m_errorReporter.typeError(0000_error, _typeClassInstantiation.location(), *error);

	return false;
}

bool TypeInference::visit(MemberAccess const& _memberAccess)
{
	if (m_expressionContext != ExpressionContext::Term)
	{
		m_errorReporter.typeError(0000_error, _memberAccess.location(), "Member access outside term context.");
		annotation(_memberAccess).type = m_typeSystem.freshTypeVariable({});
		return false;
	}
	return true;
}

void TypeInference::endVisit(MemberAccess const& _memberAccess)
{
	auto &memberAccessAnnotation = annotation(_memberAccess);
	solAssert(!memberAccessAnnotation.type);
	Type expressionType = getType(_memberAccess.expression());
	TypeSystemHelpers helper{m_typeSystem};
	if (helper.isTypeConstant(expressionType))
	{
		auto constructor = std::get<0>(helper.destTypeConstant(expressionType));
		if (auto* typeMember = util::valueOrNullptr(annotation().members.at(constructor), _memberAccess.memberName()))
		{
			Type type = m_env->fresh(typeMember->type);
			annotation(_memberAccess).type = type;
		}
		else
		{
			m_errorReporter.typeError(0000_error, _memberAccess.memberLocation(), "Member not found.");
			annotation(_memberAccess).type = m_typeSystem.freshTypeVariable({});
		}
	}
	else
	{
		m_errorReporter.typeError(0000_error, _memberAccess.expression().location(), "Unsupported member access expression.");
		annotation(_memberAccess).type = m_typeSystem.freshTypeVariable({});
	}
}

bool TypeInference::visit(TypeDefinition const& _typeDefinition)
{
	TypeSystemHelpers helper{m_typeSystem};
	auto& typeDefinitionAnnotation = annotation(_typeDefinition);
	if (typeDefinitionAnnotation.type)
		 return false;

	if (_typeDefinition.arguments())
		 _typeDefinition.arguments()->accept(*this);

	std::optional<Type> underlyingType;
	if (_typeDefinition.typeExpression())
	{
		 ScopedSaveAndRestore expressionContext{m_expressionContext, ExpressionContext::Type};
		 _typeDefinition.typeExpression()->accept(*this);
		 underlyingType = annotation(*_typeDefinition.typeExpression()).type;
	}

	vector<Type> arguments;
	if (_typeDefinition.arguments())
		 for (size_t i = 0; i < _typeDefinition.arguments()->parameters().size(); ++i)
			arguments.emplace_back(m_typeSystem.freshTypeVariable({}));

	Type definedType = type(&_typeDefinition, arguments);
	if (arguments.empty())
		 typeDefinitionAnnotation.type = definedType;
	else
		typeDefinitionAnnotation.type = helper.typeFunctionType(helper.tupleType(arguments), definedType);

	auto [members, newlyInserted] = annotation().members.emplace(typeConstructor(&_typeDefinition), map<string, TypeMember>{});
	solAssert(newlyInserted);
	if (underlyingType)
	{
		members->second.emplace("abs", TypeMember{helper.functionType(*underlyingType, definedType)});
		members->second.emplace("rep", TypeMember{helper.functionType(definedType, *underlyingType)});
	}
	return false;
}

bool TypeInference::visit(FunctionCall const&) { return true; }
void TypeInference::endVisit(FunctionCall const& _functionCall)
{
	auto& functionCallAnnotation = annotation(_functionCall);
	solAssert(!functionCallAnnotation.type);

	Type functionType = getType(_functionCall.expression());

	TypeSystemHelpers helper{m_typeSystem};
	std::vector<Type> argTypes;
	for(auto arg: _functionCall.arguments())
	{
		 switch(m_expressionContext)
		 {
		 case ExpressionContext::Term:
		 case ExpressionContext::Type:
			argTypes.emplace_back(getType(*arg));
			break;
		 case ExpressionContext::Sort:
			m_errorReporter.typeError(0000_error, _functionCall.location(), "Function call in sort context.");
			functionCallAnnotation.type = m_typeSystem.freshTypeVariable({});
			break;
		 }
	}

	switch(m_expressionContext)
	{
	case ExpressionContext::Term:
	{
		 Type genericFunctionType = helper.functionType(helper.tupleType(argTypes), m_typeSystem.freshTypeVariable({}));
		 unify(functionType, genericFunctionType, _functionCall.location());
		 functionCallAnnotation.type = m_env->resolve(std::get<1>(helper.destFunctionType(m_env->resolve(genericFunctionType))));
		 break;
	}
	case ExpressionContext::Type:
	{
		 Type argTuple = helper.tupleType(argTypes);
		 Type genericFunctionType = helper.typeFunctionType(argTuple, m_typeSystem.freshKindVariable({}));
		 unify(functionType, genericFunctionType, _functionCall.location());
		 functionCallAnnotation.type = m_env->resolve(std::get<1>(helper.destTypeFunctionType(m_env->resolve(genericFunctionType))));
		 break;
	}
	case ExpressionContext::Sort:
		 solAssert(false);
	}
}

// TODO: clean up rational parsing
namespace
{

optional<rational> parseRational(string const& _value)
{
	rational value;
	try
	{
		 auto radixPoint = find(_value.begin(), _value.end(), '.');

		 if (radixPoint != _value.end())
		 {
			if (
				!all_of(radixPoint + 1, _value.end(), util::isDigit) ||
				!all_of(_value.begin(), radixPoint, util::isDigit)
			)
				return nullopt;

			// Only decimal notation allowed here, leading zeros would switch to octal.
			auto fractionalBegin = find_if_not(
				radixPoint + 1,
				_value.end(),
				[](char const& a) { return a == '0'; }
			);

			rational numerator;
			rational denominator(1);

			denominator = bigint(string(fractionalBegin, _value.end()));
			denominator /= boost::multiprecision::pow(
				bigint(10),
				static_cast<unsigned>(distance(radixPoint + 1, _value.end()))
			);
			numerator = bigint(string(_value.begin(), radixPoint));
			value = numerator + denominator;
		 }
		 else
			value = bigint(_value);
		 return value;
	}
	catch (...)
	{
		 return nullopt;
	}
}

/// Checks whether _mantissa * (10 ** _expBase10) fits into 4096 bits.
bool fitsPrecisionBase10(bigint const& _mantissa, uint32_t _expBase10)
{
	double const log2Of10AwayFromZero = 3.3219280948873624;
	return fitsPrecisionBaseX(_mantissa, log2Of10AwayFromZero, _expBase10);
}

optional<rational> rationalValue(Literal const& _literal)
{
	rational value;
	try
	{
		 ASTString valueString = _literal.valueWithoutUnderscores();

		 auto expPoint = find(valueString.begin(), valueString.end(), 'e');
		 if (expPoint == valueString.end())
			expPoint = find(valueString.begin(), valueString.end(), 'E');

		 if (boost::starts_with(valueString, "0x"))
		 {
			// process as hex
			value = bigint(valueString);
		 }
		 else if (expPoint != valueString.end())
		 {
			// Parse mantissa and exponent. Checks numeric limit.
			optional<rational> mantissa = parseRational(string(valueString.begin(), expPoint));

			if (!mantissa)
				return nullopt;
			value = *mantissa;

			// 0E... is always zero.
			if (value == 0)
				return nullopt;

			bigint exp = bigint(string(expPoint + 1, valueString.end()));

			if (exp > numeric_limits<int32_t>::max() || exp < numeric_limits<int32_t>::min())
				return nullopt;

			uint32_t expAbs = bigint(abs(exp)).convert_to<uint32_t>();

			if (exp < 0)
			{
				if (!fitsPrecisionBase10(abs(value.denominator()), expAbs))
					return nullopt;
				value /= boost::multiprecision::pow(
					bigint(10),
					expAbs
				);
			}
			else if (exp > 0)
			{
				if (!fitsPrecisionBase10(abs(value.numerator()), expAbs))
					return nullopt;
				value *= boost::multiprecision::pow(
					bigint(10),
					expAbs
				);
			}
		 }
		 else
		 {
			// parse as rational number
			optional<rational> tmp = parseRational(valueString);
			if (!tmp)
				return nullopt;
			value = *tmp;
		 }
	}
	catch (...)
	{
		 return nullopt;
	}
	switch (_literal.subDenomination())
	{
	case Literal::SubDenomination::None:
	case Literal::SubDenomination::Wei:
	case Literal::SubDenomination::Second:
		 break;
	case Literal::SubDenomination::Gwei:
		 value *= bigint("1000000000");
		 break;
	case Literal::SubDenomination::Ether:
		 value *= bigint("1000000000000000000");
		 break;
	case Literal::SubDenomination::Minute:
		 value *= bigint("60");
		 break;
	case Literal::SubDenomination::Hour:
		 value *= bigint("3600");
		 break;
	case Literal::SubDenomination::Day:
		 value *= bigint("86400");
		 break;
	case Literal::SubDenomination::Week:
		 value *= bigint("604800");
		 break;
	case Literal::SubDenomination::Year:
		 value *= bigint("31536000");
		 break;
	}

	return value;
}
}

bool TypeInference::visit(Literal const& _literal)
{
	auto& literalAnnotation = annotation(_literal);
	if (_literal.token() != Token::Number)
	{
		 m_errorReporter.typeError(0000_error, _literal.location(), "Only number literals are supported.");
		 return false;
	}
	optional<rational> value = rationalValue(_literal);
	if (!value)
	{
		 m_errorReporter.typeError(0000_error, _literal.location(), "Invalid number literals.");
		 return false;
	}
	if (value->denominator() != 1)
	{
		 m_errorReporter.typeError(0000_error, _literal.location(), "Only integers are supported.");
		 return false;
	}
	literalAnnotation.type = m_typeSystem.freshTypeVariable(Sort{{m_analysis.annotation<TypeRegistration>().builtinClasses.at(BuiltinClass::Integer)}});
	return false;
}


namespace
{
// TODO: put at a nice place to deduplicate.
TypeRegistration::TypeClassInstantiations const& typeClassInstantiations(Analysis const& _analysis, TypeClass _class)
{
	auto const* typeClassDeclaration = _analysis.typeSystem().typeClassDeclaration(_class);
	if (typeClassDeclaration)
		 return _analysis.annotation<TypeRegistration>(*typeClassDeclaration).instantiations;
	// TODO: better mechanism than fetching by name.
	auto& annotation = _analysis.annotation<TypeRegistration>();
	return annotation.builtinClassInstantiations.at(annotation.builtinClassesByName.at(_analysis.typeSystem().typeClassName(_class)));
}
}


void TypeInference::unify(Type _a, Type _b, langutil::SourceLocation _location, TypeEnvironment* _env)
{
	TypeSystemHelpers helper{m_typeSystem};
	if (!_env)
		_env = m_env;
	auto unificationFailures = _env->unify(_a, _b);

	if (!m_activeInstantiations.empty())
	{
		// Attempt to resolve interdependencies between type class instantiations.
		std::vector<TypeClassInstantiation const*> missingInstantiations;
		bool recursion = false;
		bool onlyMissingInstantiations = [&]() {
			for (auto failure: unificationFailures)
			{
				if (auto* sortMismatch = get_if<TypeEnvironment::SortMismatch>(&failure))
					if (helper.isTypeConstant(sortMismatch->type))
					{
						TypeConstructor constructor = std::get<0>(helper.destTypeConstant(sortMismatch->type));
						for (auto typeClass: sortMismatch->sort.classes)
						{
							if (auto const* instantiation = util::valueOrDefault(typeClassInstantiations(m_analysis, typeClass), constructor, nullptr))
							{
								if (m_activeInstantiations.count(instantiation))
								{
									langutil::SecondarySourceLocation ssl;
									for (auto activeInstantiation: m_activeInstantiations)
										ssl.append("Involved instantiation", activeInstantiation->location());
									m_errorReporter.typeError(
										0000_error,
										_location,
										ssl,
										"Recursion during type class instantiation."
									);
									recursion = true;
									return false;
								}
								missingInstantiations.emplace_back(instantiation);
							}
							else
								return false;
						}
						continue;
					}
				return false;
			}
			return true;
		}();

		if (recursion)
			return;

		if (onlyMissingInstantiations)
		{
			for (auto instantiation: missingInstantiations)
				instantiation->accept(*this);
			unificationFailures = _env->unify(_a, _b);
		}
	}

	for (auto failure: unificationFailures)
	{
		TypeEnvironmentHelpers envHelper{*_env};
		std::visit(util::GenericVisitor{
			[&](TypeEnvironment::TypeMismatch _typeMismatch) {
				m_errorReporter.typeError(
					0000_error,
					_location,
					fmt::format(
						"Cannot unify {} and {}.",
						envHelper.typeToString(_typeMismatch.a),
						envHelper.typeToString(_typeMismatch.b))
				);
			},
			[&](TypeEnvironment::SortMismatch _sortMismatch) {
				m_errorReporter.typeError(0000_error, _location, fmt::format(
					"{} does not have sort {}",
					envHelper.typeToString(_sortMismatch.type),
					TypeSystemHelpers{m_typeSystem}.sortToString(_sortMismatch.sort)
				));
			},
			[&](TypeEnvironment::RecursiveUnification _recursiveUnification) {
				m_errorReporter.typeError(
					0000_error,
					_location,
					fmt::format(
						"Recursive unification: {} occurs in {}.",
						envHelper.typeToString(_recursiveUnification.var),
						envHelper.typeToString(_recursiveUnification.type))
				);
			}
		}, failure);
	}
}

experimental::Type TypeInference::getType(ASTNode const& _node) const
{
	auto result = annotation(_node).type;
	solAssert(result);
	return *result;
}
TypeConstructor TypeInference::typeConstructor(Declaration const* _type) const
{
	if (auto const& constructor = m_analysis.annotation<TypeRegistration>(*_type).typeConstructor)
		return *constructor;
	m_errorReporter.fatalTypeError(0000_error, _type->location(), "Unregistered type.");
	util::unreachable();
}
experimental::Type TypeInference::type(Declaration const* _type, vector<Type> _arguments) const
{
	return m_typeSystem.type(typeConstructor(_type), std::move(_arguments));
}

bool TypeInference::visitNode(ASTNode const& _node)
{
	m_errorReporter.fatalTypeError(0000_error, _node.location(), "Unsupported AST node during type inference.");
	return false;
}

TypeInference::Annotation& TypeInference::annotation(ASTNode const& _node)
{
	return m_analysis.annotation<TypeInference>(_node);
}

TypeInference::Annotation const& TypeInference::annotation(ASTNode const& _node) const
{
	return m_analysis.annotation<TypeInference>(_node);
}

TypeInference::GlobalAnnotation& TypeInference::annotation()
{
	return m_analysis.annotation<TypeInference>();
}
